/**
 * @file methods/reinforcement_learning/sac_impl.hpp
 * @author Nishant Kumar
 *
 * This file is the implementation of SAC class, which implements the
 * soft actor-critic algorithm.
 *
 * mlpack is free software; you may redistribute it and/or modify it under the
 * terms of the 3-clause BSD license.  You should have received a copy of the
 * 3-clause BSD license along with mlpack.  If not, see
 * http://www.opensource.org/licenses/BSD-3-Clause for more information.
 */
#ifndef MLPACK_METHODS_RL_SAC_IMPL_HPP
#define MLPACK_METHODS_RL_SAC_IMPL_HPP

#include <mlpack/prereqs.hpp>

#include "sac.hpp"

namespace mlpack {
namespace rl {

template <
  typename EnvironmentType,
  typename QNetworkType,
  typename PolicyNetworkType,
  typename UpdaterType,
  typename ReplayType
>
SAC<
  EnvironmentType,
  QNetworkType,
  PolicyNetworkType,
  UpdaterType,
  ReplayType
>::SAC(TrainingConfig& config,
       QNetworkType& learningQ1Network,
       PolicyNetworkType& policyNetwork,
       ReplayType& replayMethod,
       UpdaterType qNetworkUpdater,
       UpdaterType policyNetworkUpdater,
       EnvironmentType environment):
  config(config),
  learningQ1Network(learningQ1Network),
  policyNetwork(policyNetwork),
  replayMethod(replayMethod),
  qNetworkUpdater(std::move(qNetworkUpdater)),
  #if ENS_VERSION_MAJOR >= 2
  qNetworkUpdatePolicy(NULL),
  #endif
  policyNetworkUpdater(std::move(policyNetworkUpdater)),
  #if ENS_VERSION_MAJOR >= 2
  policyNetworkUpdatePolicy(NULL),
  #endif
  environment(std::move(environment)),
  totalSteps(0),
  deterministic(false)
{
  // Set up q-learning and policy networks.
  if (learningQ1Network.Parameters().is_empty())
    learningQ1Network.ResetParameters();
  learningQ2Network = learningQ1Network;
  learningQ2Network.ResetParameters();
  if (policyNetwork.Parameters().is_empty())
    policyNetwork.ResetParameters();

  #if ENS_VERSION_MAJOR == 1
  this->qNetworkUpdater.Initialize(learningQ1Network.Parameters().n_rows,
                                   learningQ1Network.Parameters().n_cols);
  #else
  this->qNetworkUpdatePolicy = new typename UpdaterType::template
      Policy<arma::mat, arma::mat>(this->qNetworkUpdater,
                                   learningQ1Network.Parameters().n_rows,
                                   learningQ1Network.Parameters().n_cols);
  #endif

  #if ENS_VERSION_MAJOR == 1
  this->policyNetworkUpdater.Initialize(policyNetwork.Parameters().n_rows,
                                        policyNetwork.Parameters().n_cols);
  #else
  this->policyNetworkUpdatePolicy = new typename UpdaterType::template
      Policy<arma::mat, arma::mat>(this->policyNetworkUpdater,
                                   policyNetwork.Parameters().n_rows,
                                   policyNetwork.Parameters().n_cols);
  #endif

  targetQ1Network = learningQ1Network;
  targetQ2Network = learningQ2Network;
}

template <
  typename EnvironmentType,
  typename QNetworkType,
  typename PolicyNetworkType,
  typename UpdaterType,
  typename ReplayType
>
SAC<
  EnvironmentType,
  QNetworkType,
  PolicyNetworkType,
  UpdaterType,
  ReplayType
>::~SAC()
{
  #if ENS_VERSION_MAJOR >= 2
  delete qNetworkUpdatePolicy;
  delete policyNetworkUpdatePolicy;
  #endif
}

template <
  typename EnvironmentType,
  typename QNetworkType,
  typename PolicyNetworkType,
  typename UpdaterType,
  typename ReplayType
>
void SAC<
  EnvironmentType,
  QNetworkType,
  PolicyNetworkType,
  UpdaterType,
  ReplayType
>::SoftUpdate(double rho)
{
  targetQ1Network.Parameters() = (1 - rho) * targetQ1Network.Parameters() +
      rho * learningQ1Network.Parameters();
  targetQ2Network.Parameters() = (1 - rho) * targetQ2Network.Parameters() +
      rho * learningQ2Network.Parameters();
}

template <
  typename EnvironmentType,
  typename QNetworkType,
  typename PolicyNetworkType,
  typename UpdaterType,
  typename ReplayType
>
void SAC<
  EnvironmentType,
  QNetworkType,
  PolicyNetworkType,
  UpdaterType,
  ReplayType
>::Update()
{
  // Sample from previous experience.
  arma::mat sampledStates;
  std::vector<ActionType> sampledActions;
  arma::rowvec sampledRewards;
  arma::mat sampledNextStates;
  arma::irowvec isTerminal;

  replayMethod.Sample(sampledStates, sampledActions, sampledRewards,
      sampledNextStates, isTerminal);

  // Critic network update.

  // Get the actions for sampled next states, from policy.
  arma::mat nextStateActions;
  policyNetwork.Predict(sampledNextStates, nextStateActions);

  arma::mat targetQInput = arma::join_vert(nextStateActions,
      sampledNextStates);
  arma::rowvec Q1, Q2;
  targetQ1Network.Predict(targetQInput, Q1);
  targetQ2Network.Predict(targetQInput, Q2);
  arma::rowvec nextQ = sampledRewards +  config.Discount() * (1 - isTerminal)
      % arma::min(Q1, Q2);

  arma::mat sampledActionValues(action.size, sampledActions.size());
  for (size_t i = 0; i < sampledActions.size(); i++)
    sampledActionValues.col(i) = sampledActions[i].action[0];
  arma::mat learningQInput = arma::join_vert(sampledActionValues,
      sampledStates);
  learningQ1Network.Forward(learningQInput, Q1);
  learningQ2Network.Forward(learningQInput, Q2);

  arma::mat gradQ1Loss, gradQ2Loss;
  lossFunction.Backward(Q1, nextQ, gradQ1Loss);
  lossFunction.Backward(Q2, nextQ, gradQ2Loss);

  // Update the critic networks.
  arma::mat gradientQ1, gradientQ2;
  learningQ1Network.Backward(learningQInput, gradQ1Loss, gradientQ1);
  #if ENS_VERSION_MAJOR == 1
  qNetworkUpdater.Update(learningQ1Network.Parameters(), config.StepSize(),
      gradientQ1);
  #else
  qNetworkUpdatePolicy->Update(learningQ1Network.Parameters(),
      config.StepSize(), gradientQ1);
  #endif
  learningQ2Network.Backward(learningQInput, gradQ2Loss, gradientQ2);
  #if ENS_VERSION_MAJOR == 1
  qNetworkUpdater.Update(learningQ2Network.Parameters(), config.StepSize(),
      gradientQ2);
  #else
  qNetworkUpdatePolicy->Update(learningQ2Network.Parameters(),
      config.StepSize(), gradientQ2);
  #endif

  // Actor network update.

  arma::rowvec pi;
  policyNetwork.Predict(sampledStates, pi);

  arma::mat qInput = arma::join_vert(pi, sampledStates);
  learningQ1Network.Predict(qInput, Q1);
  learningQ2Network.Predict(qInput, Q2);

  arma::mat gradient;
  for (size_t i = 0; i < sampledStates.n_cols; i++)
  {
    arma::mat grad, gradQ;
    arma::mat q;
    arma::colvec singleState = sampledStates.col(i);
    arma::colvec singlePi = pi.col(i);
    policyNetwork.Forward(singleState, singlePi);
    arma::colvec input = arma::join_vert(singlePi, singleState);
    if (Q1(i) < Q2(i))
    {
      learningQ1Network.Forward(input, q);
      learningQ1Network.Backward(input, -q, gradQ);
    }
    else
    {
      learningQ2Network.Forward(input, q);
      learningQ2Network.Backward(input, -q, gradQ);
    }
    //! TODO: Check for the correct location of pi grad.
    arma::mat gradPolicy = gradQ.rows(0, singlePi.n_rows - 1);
    policyNetwork.Backward(singleState, gradPolicy, grad);
    if (i == 0)
    {
      gradient.copy_size(grad);
      gradient.fill(0.0);
    }
    gradient += grad;
  }
  // gradient /= sampledStates.n_cols;

  #if ENS_VERSION_MAJOR == 1
  policyUpdater.Update(policyNetwork.Parameters(), config.StepSize(), gradient);
  #else
  policyNetworkUpdatePolicy->Update(policyNetwork.Parameters(),
      config.StepSize(), gradient);
  #endif

  // Update target network
  if (totalSteps % config.TargetNetworkSyncInterval() == 0)
    SoftUpdate(0.005);
}

template <
  typename EnvironmentType,
  typename QNetworkType,
  typename PolicyNetworkType,
  typename UpdaterType,
  typename ReplayType
>
void SAC<
  EnvironmentType,
  QNetworkType,
  PolicyNetworkType,
  UpdaterType,
  ReplayType
>::SelectAction()
{
  // Get the action at current state, from policy.
  arma::rowvec outputAction;
  policyNetwork.Predict(state.Encode(), outputAction);

  if (!deterministic)
  {
    arma::rowvec noise = arma::randu<arma::rowvec>(outputAction.n_rows) * 0.1;
    noise = arma::clamp(noise, -0.25, 0.25);
    outputAction = outputAction + noise;
  }
  action.action[0] = outputAction[0];
}

template <
  typename EnvironmentType,
  typename QNetworkType,
  typename PolicyNetworkType,
  typename UpdaterType,
  typename ReplayType
>
double SAC<
  EnvironmentType,
  QNetworkType,
  PolicyNetworkType,
  UpdaterType,
  ReplayType
>::Episode()
{
  // Get the initial state from environment.
  state = environment.InitialSample();

  // Track the steps in this episode.
  size_t steps = 0;

  // Track the return of this episode.
  double totalReturn = 0.0;

  // Running until get to the terminal state.
  while (!environment.IsTerminal(state))
  {
    if (config.StepLimit() && steps >= config.StepLimit())
      break;
    SelectAction();

    // Interact with the environment to advance to next state.
    StateType nextState;
    double reward = environment.Sample(state, action, nextState);

    totalReturn += reward;
    steps++;
    totalSteps++;

    // Store the transition for replay.
    replayMethod.Store(state, action, reward, nextState,
        environment.IsTerminal(nextState), config.Discount());

    // Update current state.
    state = nextState;

    if (deterministic || totalSteps < config.ExplorationSteps())
      continue;
    Update();
  }

  return totalReturn;
}

} // namespace rl
} // namespace mlpack
#endif
