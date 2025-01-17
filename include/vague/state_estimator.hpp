#pragma once

#include "Eigen/LU"
#include "vague/estimate.hpp"
#include "vague/unscented_transform.hpp"
#include "vague/utility.hpp"

#include <chrono>

namespace vague {

template <typename StateSpaceT, typename ObservationSpaceT, typename ScalarT>
struct PredictedObservation : MeanAndCovariance<ObservationSpaceT, ScalarT> {
    using StateSpace = StateSpaceT;
    using ObservationSpace = ObservationSpaceT;
    using Scalar = ScalarT;

    // TODO: Make this optional? So we don't need to calculate it unless its actually used for an update
    //       (eg. predicted observation might just be used to perform association)
    //       Will probably need to be to a variant so that the original state can be copied in?
    Eigen::Matrix<Scalar, StateSpace::N, ObservationSpace::N> cross_covariance;
};

template <typename StateSpaceT, typename ScalarT>
struct TimeDependentAdditiveProcessNoise {
    using StateSpace = StateSpaceT;
    using Scalar = ScalarT;
    using Mean = Eigen::Matrix<Scalar, StateSpace::N, 1>;
    using Covariance = Eigen::Matrix<Scalar, StateSpace::N, StateSpace::N>;
    TimeDependentAdditiveProcessNoise(const Covariance& process_noise_per_second) :
        process_noise_per_second(process_noise_per_second) { }
    Covariance& operator()(const Scalar& dt, const Mean&, Covariance& covariance) const {
        return (covariance += dt * process_noise_per_second);
    }
    Covariance process_noise_per_second;
};

template <typename StateSpaceT, typename ScalarT, typename TimePointT>
class StateEstimator {
public:
    using StateSpace = StateSpaceT;
    using Scalar = ScalarT;
    using TimePoint = TimePointT;

    StateEstimator(const TimePoint& initial_time, const MeanAndCovariance<StateSpace, Scalar>& initial_estimate) noexcept :
        time(initial_time),
        estimate(initial_estimate) { }

    template <typename Dynamics, typename ProcessNoise>
    void predict(const TimePoint& t, const Dynamics& dynamics, const ProcessNoise& process_noise) {
        const auto duration = (t - time);

        if (duration.count() == 0) { return; }
        if (duration.count() < 0) { throw std::runtime_error("Unable to wind back time"); }

        time = t;
        Scalar dt = std::chrono::duration_cast<std::chrono::duration<Scalar>>(duration).count();

        if constexpr (vague::utility::FunctionTypeCallableWith<decltype(estimate), decltype(dt)>::value) {
            // Path for linear & differentiable dynamics
            estimate = dynamics(estimate, dt);
        } else {
            // Need to sample sigma points for non-linear & non-differentiable dynamics
            estimate = dynamics(sample(estimate, unscented_transform::CubatureSigmaPoints()), dt).statistics();
        }
        estimate.covariance =
            process_noise(dt,
                          // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) Add const; prevent mutation of mean
                          const_cast<const Eigen::Vector<Scalar, StateSpace::N>&>(estimate.mean),
                          estimate.covariance);
    }

    template <typename Observer, typename... AugmentedState>
    PredictedObservation<StateSpace, typename Observer::To, Scalar>
    predict_observation(const Observer& observer, const AugmentedState&... augmented_state) const noexcept {
        if constexpr (vague::utility::FunctionTypeCallableWith<decltype(estimate), AugmentedState...>::value) {
            // Path for linear & differentiable dynamics
            return {observer(estimate, augmented_state...),
                    estimate.covariance * observer.jacobian(estimate, augmented_state...).transpose()};
        } else {
            // Need to sample sigma points for non-linear & non-differentiable dynamics
            const auto sigma_points = sample(estimate, unscented_transform::CubatureSigmaPoints());
            const auto& [state_mean, state_centered_samples] = sigma_points.mean_centered_samples();

            const auto predicted_obs_sigma_points = observer(sigma_points, augmented_state...);
            const auto& [predicted_obs_mean, predicted_obs_centered_samples] =
                predicted_obs_sigma_points.mean_centered_samples();
            return {predicted_obs_sigma_points.statistics(),
                    state_centered_samples * sigma_points.weights.asDiagonal()
                        * predicted_obs_centered_samples.transpose()};
        }
    }

    template <typename ObservationSpace>
    void assimilate(const PredictedObservation<StateSpace, ObservationSpace, Scalar>& predicted_observation,
                    const MeanAndCovariance<ObservationSpace, Scalar>& observation) noexcept {
        using ObservationCovariance = Eigen::Matrix<Scalar, ObservationSpace::N, ObservationSpace::N>;
        const ObservationCovariance observation_covariance_sum = predicted_observation.covariance + observation.covariance;
        Eigen::PartialPivLU<ObservationCovariance> lu_solver(observation_covariance_sum);
        const Eigen::Matrix<Scalar, StateSpace::N, ObservationSpace::N> kalman_gain =
            // Below line is equivalent to: predicted_observation.cross_covariance * observation_covariance_sum.inverse();
            predicted_observation.cross_covariance * lu_solver.inverse();
        estimate.mean += kalman_gain * (observation.mean - predicted_observation.mean);
        estimate.covariance -= kalman_gain * observation_covariance_sum * kalman_gain.transpose();
    }

    TimePoint time;
    MeanAndCovariance<StateSpace, Scalar> estimate;
};

template <typename TimePoint, typename Estimate>
StateEstimator(TimePoint, Estimate)
    -> StateEstimator<typename Estimate::StateSpace, typename Estimate::Scalar, TimePoint>;

} // namespace vague