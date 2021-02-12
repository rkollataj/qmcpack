//////////////////////////////////////////////////////////////////////////////////////
// This file is distributed under the University of Illinois/NCSA Open Source License.
// See LICENSE file in top directory for details.
//
// Copyright (c) 2020 QMCPACK developers.
//
// File developed by: Bryan Clark, bclark@Princeton.edu, Princeton University
//                    Ken Esler, kpesler@gmail.com, University of Illinois at Urbana-Champaign
//                    Miguel Morales, moralessilva2@llnl.gov, Lawrence Livermore National Laboratory
//                    Jeremy McMinnis, jmcminis@gmail.com, University of Illinois at Urbana-Champaign
//                    Jeongnim Kim, jeongnim.kim@gmail.com, University of Illinois at Urbana-Champaign
//                    Jaron T. Krogel, krogeljt@ornl.gov, Oak Ridge National Laboratory
//                    Raymond Clay III, j.k.rofling@gmail.com, Lawrence Livermore National Laboratory
//                    Mark A. Berrill, berrillma@ornl.gov, Oak Ridge National Laboratory
//
// File created by: Jeongnim Kim, jeongnim.kim@gmail.com, University of Illinois at Urbana-Champaign
//////////////////////////////////////////////////////////////////////////////////////

#include <stdexcept>

#include "TrialWaveFunction.h"
#include "ResourceCollection.h"
#include "Utilities/IteratorUtility.h"
#include "Concurrency/Info.hpp"

namespace qmcplusplus
{
typedef enum
{
  V_TIMER = 0,
  VGL_TIMER,
  ACCEPT_TIMER,
  NL_TIMER,
  RECOMPUTE_TIMER,
  BUFFER_TIMER,
  DERIVS_TIMER,
  PREPAREGROUP_TIMER,
  TIMER_SKIP
} TimerEnum;

static const std::vector<std::string> suffixes{"V",         "VGL",    "accept", "NLratio",
                                               "recompute", "buffer", "derivs", "preparegroup"};

TrialWaveFunction::TrialWaveFunction(const std::string& aname, bool tasking, bool create_local_resource)
    : myName(aname),
      BufferCursor(0),
      BufferCursor_scalar(0),
      PhaseValue(0.0),
      PhaseDiff(0.0),
      LogValue(0.0),
      OneOverM(1.0),
      use_tasking_(tasking)
{
  if (suffixes.size() != TIMER_SKIP)
    throw std::runtime_error("TrialWaveFunction::TrialWaveFunction mismatched timer enums and suffixes");
  if (create_local_resource)
    twf_resource_ = std::make_unique<ResourceCollection>("TrialWaveFunction");

  for (auto& suffix : suffixes)
  {
    std::string timer_name = "WaveFunction:" + myName + "::" + suffix;
    TWF_timers_.push_back(timer_manager.createTimer(timer_name, timer_level_medium));
  }
}

/** Destructor
*
*@warning Have not decided whether Z is cleaned up by TrialWaveFunction
*  or not. It will depend on I/O implementation.
*/
TrialWaveFunction::~TrialWaveFunction() { delete_iter(Z.begin(), Z.end()); }

void TrialWaveFunction::startOptimization()
{
  for (int i = 0; i < Z.size(); i++)
    Z[i]->IsOptimizing = true;
}

void TrialWaveFunction::stopOptimization()
{
  for (int i = 0; i < Z.size(); i++)
  {
    Z[i]->finalizeOptimization();
    Z[i]->IsOptimizing = false;
  }
}

/** Takes owndership of aterm
 */
void TrialWaveFunction::addComponent(WaveFunctionComponent* aterm)
{
  if (twf_resource_)
    aterm->createResource(*twf_resource_);
  Z.push_back(aterm);

  std::string aname = aterm->ClassName;
  if (!aterm->myName.empty())
    aname += ":" + aterm->myName;

  if (aterm->is_fermionic)
    app_log() << "  Added a fermionic WaveFunctionComponent " << aname << std::endl;

  for (auto& suffix : suffixes)
    WFC_timers_.push_back(timer_manager.createTimer(aname + "::" + suffix));
}


/** return log(|psi|)
*
* PhaseValue is the phase for the complex wave function
*/
TrialWaveFunction::RealType TrialWaveFunction::evaluateLog(ParticleSet& P)
{
  ScopedTimer local_timer(TWF_timers_[RECOMPUTE_TIMER]);
  P.G = 0.0;
  P.L = 0.0;
  LogValueType logpsi(0.0);
  for (int i = 0, ii = RECOMPUTE_TIMER; i < Z.size(); ++i, ii += TIMER_SKIP)
  {
    ScopedTimer z_timer(WFC_timers_[ii]);
#ifndef NDEBUG
    // Best way I've found yet to quickly see if WFC made it over the wire successfully
    auto subterm = Z[i]->evaluateLog(P, P.G, P.L);
    // std::cerr << "evaluate log Z element:" <<  i << "  value: " << subterm << '\n';
    logpsi += subterm;
#else
    logpsi += Z[i]->evaluateLog(P, P.G, P.L);
#endif
  }

  G = P.G;
  L = P.L;

  LogValue   = std::real(logpsi);
  PhaseValue = std::imag(logpsi);
  return LogValue;
}

void TrialWaveFunction::flex_evaluateLog(const RefVector<TrialWaveFunction>& wf_list,
                                         const RefVector<ParticleSet>& p_list)
{
  if (wf_list.size() > 1)
  {
    ScopedTimer local_timer(wf_list[0].get().TWF_timers_[RECOMPUTE_TIMER]);

    constexpr RealType czero(0);
    const auto g_list(TrialWaveFunction::extractGRefList(wf_list));
    const auto l_list(TrialWaveFunction::extractLRefList(wf_list));

    int num_particles = p_list[0].get().getTotalNum();
    auto initGandL    = [num_particles, czero](TrialWaveFunction& twf, ParticleSet::ParticleGradient_t& grad,
                                            ParticleSet::ParticleLaplacian_t& lapl) {
      grad.resize(num_particles);
      lapl.resize(num_particles);
      grad           = czero;
      lapl           = czero;
      twf.LogValue   = czero;
      twf.PhaseValue = czero;
    };
    for (int iw = 0; iw < wf_list.size(); iw++)
      initGandL(wf_list[iw], g_list[iw], l_list[iw]);

    auto& wavefunction_components = wf_list[0].get().Z;
    const int num_wfc             = wf_list[0].get().Z.size();
    for (int i = 0, ii = RECOMPUTE_TIMER; i < num_wfc; ++i, ii += TIMER_SKIP)
    {
      ScopedTimer z_timer(wf_list[0].get().WFC_timers_[ii]);
      const auto wfc_list(extractWFCRefList(wf_list, i));
      wavefunction_components[i]->mw_evaluateLog(wfc_list, p_list, g_list, l_list);
      auto accumulateLogAndPhase = [](TrialWaveFunction& twf, WaveFunctionComponent& wfc) {
        twf.LogValue += std::real(wfc.LogValue);
        twf.PhaseValue += std::imag(wfc.LogValue);
      };
      for (int iw = 0; iw < wf_list.size(); iw++)
        accumulateLogAndPhase(wf_list[iw], wfc_list[iw]);
    }
    auto copyToP = [](ParticleSet& pset, TrialWaveFunction& twf) {
      pset.G = twf.G;
      pset.L = twf.L;
    };
    // Ye: temporal workaround to have P.G/L always defined.
    // remove when KineticEnergy use WF.G/L instead of P.G/L
    for (int iw = 0; iw < wf_list.size(); iw++)
      copyToP(p_list[iw], wf_list[iw]);
  }
  else if (wf_list.size() == 1)
    wf_list[0].get().evaluateLog(p_list[0]);
}

void TrialWaveFunction::recompute(ParticleSet& P)
{
  ScopedTimer local_timer(TWF_timers_[RECOMPUTE_TIMER]);
  std::vector<WaveFunctionComponent*>::iterator it(Z.begin());
  std::vector<WaveFunctionComponent*>::iterator it_end(Z.end());
  for (int ii = RECOMPUTE_TIMER; it != it_end; ++it, ii += TIMER_SKIP)
  {
    ScopedTimer z_timer(WFC_timers_[ii]);
    (*it)->recompute(P);
  }
}


TrialWaveFunction::RealType TrialWaveFunction::evaluateDeltaLog(ParticleSet& P, bool recomputeall)
{
  ScopedTimer local_timer(TWF_timers_[RECOMPUTE_TIMER]);
  P.G = 0.0;
  P.L = 0.0;
  LogValueType logpsi(0.0);
  std::vector<WaveFunctionComponent*>::iterator it(Z.begin());
  std::vector<WaveFunctionComponent*>::iterator it_end(Z.end());
  int ii = RECOMPUTE_TIMER;
  for (; it != it_end; ++it, ii += TIMER_SKIP)
  {
    ScopedTimer z_timer(WFC_timers_[ii]);
    if ((*it)->Optimizable)
      logpsi += (*it)->evaluateLog(P, P.G, P.L);
  }
  LogValue   = std::real(logpsi);
  PhaseValue = std::imag(logpsi);

  //In case we need to recompute orbitals, initialize dummy vectors for G and L.
  //evaluateLog dumps into these variables, and logPsi contribution is discarded.
  //Only called for non-optimizable orbitals.
  if (recomputeall)
  {
    ParticleSet::ParticleGradient_t dummyG(P.G);
    ParticleSet::ParticleLaplacian_t dummyL(P.L);

    it     = Z.begin();
    it_end = Z.end();

    for (; it != it_end; ++it)
    {
      if (!(*it)->Optimizable)
        (*it)->evaluateLog(P, dummyG,
                           dummyL); //update orbitals if its not flagged optimizable, AND recomputeall is true
    }
  }
  return LogValue;
}

void TrialWaveFunction::evaluateDeltaLog(ParticleSet& P,
                                         RealType& logpsi_fixed_r,
                                         RealType& logpsi_opt_r,
                                         ParticleSet::ParticleGradient_t& fixedG,
                                         ParticleSet::ParticleLaplacian_t& fixedL)
{
  ScopedTimer local_timer(TWF_timers_[RECOMPUTE_TIMER]);
  P.G    = 0.0;
  P.L    = 0.0;
  fixedL = 0.0;
  fixedG = 0.0;
  LogValueType logpsi_fixed(0.0);
  LogValueType logpsi_opt(0.0);
  std::vector<WaveFunctionComponent*>::iterator it(Z.begin());
  std::vector<WaveFunctionComponent*>::iterator it_end(Z.end());
  int ii = RECOMPUTE_TIMER;
  for (; it != it_end; ++it, ii += TIMER_SKIP)
  {
    ScopedTimer z_timer(WFC_timers_[ii]);
    if ((*it)->Optimizable)
      logpsi_opt += (*it)->evaluateLog(P, P.G, P.L);
    else
      logpsi_fixed += (*it)->evaluateLog(P, fixedG, fixedL);
  }
  P.G += fixedG;
  P.L += fixedL;
  convert(logpsi_fixed, logpsi_fixed_r);
  convert(logpsi_opt, logpsi_opt_r);
}


void TrialWaveFunction::flex_evaluateDeltaLogSetup(const RefVector<TrialWaveFunction>& wf_list,
                                                   const RefVector<ParticleSet>& p_list,
                                                   std::vector<RealType>& logpsi_fixed_list,
                                                   std::vector<RealType>& logpsi_opt_list,
                                                   RefVector<ParticleSet::ParticleGradient_t>& fixedG_list,
                                                   RefVector<ParticleSet::ParticleLaplacian_t>& fixedL_list)
{
  ScopedTimer local_timer(wf_list[0].get().TWF_timers_[RECOMPUTE_TIMER]);
  constexpr RealType czero(0);
  int num_particles = p_list[0].get().getTotalNum();
  const auto g_list(TrialWaveFunction::extractGRefList(wf_list));
  const auto l_list(TrialWaveFunction::extractLRefList(wf_list));

  auto initGandL = [num_particles, czero](TrialWaveFunction& twf, ParticleSet::ParticleGradient_t& grad,
                                          ParticleSet::ParticleLaplacian_t& lapl) {
    grad.resize(num_particles);
    lapl.resize(num_particles);
    grad           = czero;
    lapl           = czero;
    twf.LogValue   = czero;
    twf.PhaseValue = czero;
  };
  for (int iw = 0; iw < wf_list.size(); iw++)
    initGandL(wf_list[iw], g_list[iw], l_list[iw]);
  auto& wavefunction_components = wf_list[0].get().Z;
  const int num_wfc             = wf_list[0].get().Z.size();
  for (int i = 0, ii = RECOMPUTE_TIMER; i < num_wfc; ++i, ii += TIMER_SKIP)
  {
    ScopedTimer z_timer(wf_list[0].get().WFC_timers_[ii]);
    const auto wfc_list(extractWFCRefList(wf_list, i));
    if (wavefunction_components[i]->Optimizable)
    {
      wavefunction_components[i]->mw_evaluateLog(wfc_list, p_list, g_list, l_list);
      for (int iw = 0; iw < wf_list.size(); iw++)
        logpsi_opt_list[iw] += std::real(wfc_list[iw].get().LogValue);
    }
    else
    {
      wavefunction_components[i]->mw_evaluateLog(wfc_list, p_list, fixedG_list, fixedL_list);
      for (int iw = 0; iw < wf_list.size(); iw++)
        logpsi_fixed_list[iw] += std::real(wfc_list[iw].get().LogValue);
    }
  }

  // Temporary workaround to have P.G/L always defined.
  // remove when KineticEnergy use WF.G/L instead of P.G/L
  auto addAndCopyToP = [](ParticleSet& pset, TrialWaveFunction& twf, ParticleSet::ParticleGradient_t& grad,
                          ParticleSet::ParticleLaplacian_t& lapl) {
    pset.G = twf.G + grad;
    pset.L = twf.L + lapl;
  };
  for (int iw = 0; iw < wf_list.size(); iw++)
    addAndCopyToP(p_list[iw], wf_list[iw], fixedG_list[iw], fixedL_list[iw]);
}


void TrialWaveFunction::flex_evaluateDeltaLog(const RefVector<TrialWaveFunction>& wf_list,
                                              const RefVector<ParticleSet>& p_list,
                                              std::vector<RealType>& logpsi_list,
                                              RefVector<ParticleSet::ParticleGradient_t>& dummyG_list,
                                              RefVector<ParticleSet::ParticleLaplacian_t>& dummyL_list,
                                              bool recompute)
{
  ScopedTimer local_timer(wf_list[0].get().TWF_timers_[RECOMPUTE_TIMER]);
  constexpr RealType czero(0);
  int num_particles = p_list[0].get().getTotalNum();
  const auto g_list(TrialWaveFunction::extractGRefList(wf_list));
  const auto l_list(TrialWaveFunction::extractLRefList(wf_list));

  // Initialize various members of the wavefunction, grad, and laplacian
  auto initGandL = [num_particles, czero](TrialWaveFunction& twf, ParticleSet::ParticleGradient_t& grad,
                                          ParticleSet::ParticleLaplacian_t& lapl) {
    grad.resize(num_particles);
    lapl.resize(num_particles);
    grad           = czero;
    lapl           = czero;
    twf.LogValue   = czero;
    twf.PhaseValue = czero;
  };
  for (int iw = 0; iw < wf_list.size(); iw++)
    initGandL(wf_list[iw], g_list[iw], l_list[iw]);

  // Get wavefunction components (assumed the same for every WF in the list)
  auto& wavefunction_components = wf_list[0].get().Z;
  const int num_wfc             = wf_list[0].get().Z.size();

  // Loop over the wavefunction components
  for (int i = 0, ii = RECOMPUTE_TIMER; i < num_wfc; ++i, ii += TIMER_SKIP)
    if (wavefunction_components[i]->Optimizable)
    {
      ScopedTimer z_timer(wf_list[0].get().WFC_timers_[ii]);
      const auto wfc_list(extractWFCRefList(wf_list, i));
      wavefunction_components[i]->mw_evaluateLog(wfc_list, p_list, g_list, l_list);
      for (int iw = 0; iw < wf_list.size(); iw++)
        logpsi_list[iw] += std::real(wfc_list[iw].get().LogValue);
    }

  // Temporary workaround to have P.G/L always defined.
  // remove when KineticEnergy use WF.G/L instead of P.G/L
  auto copyToP = [](ParticleSet& pset, TrialWaveFunction& twf) {
    pset.G = twf.G;
    pset.L = twf.L;
  };
  for (int iw = 0; iw < wf_list.size(); iw++)
    copyToP(p_list[iw], wf_list[iw]);

  // Recompute is usually used to prepare the wavefunction for NLPP derivatives.
  // (e.g compute the matrix inverse for determinants)
  // Call mw_evaluateLog for the wavefunction components that were skipped previously.
  // Ignore logPsi, G and L.
  if (recompute)
    for (int i = 0, ii = RECOMPUTE_TIMER; i < num_wfc; ++i, ii += TIMER_SKIP)
      if (!wavefunction_components[i]->Optimizable)
      {
        ScopedTimer z_timer(wf_list[0].get().WFC_timers_[ii]);
        const auto wfc_list(extractWFCRefList(wf_list, i));
        wavefunction_components[i]->mw_evaluateLog(wfc_list, p_list, dummyG_list, dummyL_list);
      }
}


/*void TrialWaveFunction::evaluateHessian(ParticleSet & P, int iat, HessType& grad_grad_psi)
{
  std::vector<WaveFunctionComponent*>::iterator it(Z.begin());
  std::vector<WaveFunctionComponent*>::iterator it_end(Z.end());
  
  grad_grad_psi=0.0;
  
  for (; it!=it_end; ++it)
  {	
	  HessType tmp_hess;
	  (*it)->evaluateHessian(P, iat, tmp_hess);
	  grad_grad_psi+=tmp_hess;
  }
}*/

void TrialWaveFunction::evaluateHessian(ParticleSet& P, HessVector_t& grad_grad_psi)
{
  std::vector<WaveFunctionComponent*>::iterator it(Z.begin());
  std::vector<WaveFunctionComponent*>::iterator it_end(Z.end());

  grad_grad_psi.resize(P.getTotalNum());

  for (int i = 0; i < Z.size(); i++)
  {
    HessVector_t tmp_hess(grad_grad_psi);
    tmp_hess = 0.0;
    Z[i]->evaluateHessian(P, tmp_hess);
    grad_grad_psi += tmp_hess;
    //  app_log()<<"TrialWavefunction::tmp_hess = "<<tmp_hess<< std::endl;
    //  app_log()<< std::endl<< std::endl;
  }
  // app_log()<<" TrialWavefunction::Hessian = "<<grad_grad_psi<< std::endl;
}

TrialWaveFunction::ValueType TrialWaveFunction::calcRatio(ParticleSet& P, int iat, ComputeType ct)
{
  ScopedTimer local_timer(TWF_timers_[V_TIMER]);
  PsiValueType r(1.0);
  for (int i = 0, ii = V_TIMER; i < Z.size(); i++, ii += TIMER_SKIP)
    if (ct == ComputeType::ALL || (Z[i]->is_fermionic && ct == ComputeType::FERMIONIC) ||
        (!Z[i]->is_fermionic && ct == ComputeType::NONFERMIONIC))
    {
      ScopedTimer z_timer(WFC_timers_[ii]);
      r *= Z[i]->ratio(P, iat);
    }
  return static_cast<ValueType>(r);
}

void TrialWaveFunction::flex_calcRatio(const RefVector<TrialWaveFunction>& wf_list,
                                       const RefVector<ParticleSet>& p_list,
                                       int iat,
                                       std::vector<PsiValueType>& ratios,
                                       ComputeType ct)
{
  const int num_wf = wf_list.size();
  ratios.resize(num_wf);
  std::fill(ratios.begin(), ratios.end(), PsiValueType(1));

  if (num_wf > 1)
  {
    ScopedTimer local_timer(wf_list[0].get().TWF_timers_[V_TIMER]);
    const int num_wfc             = wf_list[0].get().Z.size();
    auto& wavefunction_components = wf_list[0].get().Z;

    std::vector<PsiValueType> ratios_z(num_wf);
    for (int i = 0, ii = V_TIMER; i < num_wfc; i++, ii += TIMER_SKIP)
    {
      if (ct == ComputeType::ALL || (wavefunction_components[i]->is_fermionic && ct == ComputeType::FERMIONIC) ||
          (!wavefunction_components[i]->is_fermionic && ct == ComputeType::NONFERMIONIC))
      {
        ScopedTimer z_timer(wf_list[0].get().WFC_timers_[ii]);
        const auto wfc_list(extractWFCRefList(wf_list, i));
        wavefunction_components[i]->mw_calcRatio(wfc_list, p_list, iat, ratios_z);
        for (int iw = 0; iw < wf_list.size(); iw++)
          ratios[iw] *= ratios_z[iw];
      }
    }
    for (int iw = 0; iw < wf_list.size(); iw++)
      wf_list[iw].get().PhaseDiff = std::imag(std::arg(ratios[iw]));
  }
  else if (wf_list.size() == 1)
    ratios[0] = wf_list[0].get().calcRatio(p_list[0], iat);
}

void TrialWaveFunction::prepareGroup(ParticleSet& P, int ig)
{
  for (int i = 0; i < Z.size(); ++i)
    Z[i]->prepareGroup(P, ig);
}

void TrialWaveFunction::flex_prepareGroup(const RefVector<TrialWaveFunction>& wf_list,
                                          const RefVector<ParticleSet>& P_list,
                                          int ig)
{
  if (wf_list.size() > 1)
  {
    ScopedTimer local_timer(wf_list[0].get().TWF_timers_[PREPAREGROUP_TIMER]);
    const int num_wfc             = wf_list[0].get().Z.size();
    auto& wavefunction_components = wf_list[0].get().Z;

    for (int i = 0, ii = PREPAREGROUP_TIMER; i < num_wfc; i++, ii += TIMER_SKIP)
    {
      ScopedTimer z_timer(wf_list[0].get().WFC_timers_[ii]);
      const auto wfc_list(extractWFCRefList(wf_list, i));
      wavefunction_components[i]->mw_prepareGroup(wfc_list, P_list, ig);
    }
  }
  else if (wf_list.size() == 1)
  {
    for (int i = 0; i < wf_list[0].get().Z.size(); ++i)
      wf_list[0].get().Z[i]->prepareGroup(P_list[0], ig);
  }
}

TrialWaveFunction::GradType TrialWaveFunction::evalGrad(ParticleSet& P, int iat)
{
  ScopedTimer local_timer(TWF_timers_[VGL_TIMER]);
  GradType grad_iat;
  for (int i = 0, ii = VGL_TIMER; i < Z.size(); ++i, ii += TIMER_SKIP)
  {
    ScopedTimer z_timer(WFC_timers_[ii]);
    grad_iat += Z[i]->evalGrad(P, iat);
  }
  return grad_iat;
}

TrialWaveFunction::GradType TrialWaveFunction::evalGradWithSpin(ParticleSet& P, int iat, ComplexType& spingrad)
{
  ScopedTimer local_timer(TWF_timers_[VGL_TIMER]);
  GradType grad_iat;
  spingrad = 0;
  for (int i = 0, ii = VGL_TIMER; i < Z.size(); ++i, ii += TIMER_SKIP)
  {
    ScopedTimer z_timer(WFC_timers_[ii]);
    grad_iat += Z[i]->evalGradWithSpin(P, iat, spingrad);
  }
  return grad_iat;
}

void TrialWaveFunction::flex_evalGrad(const RefVector<TrialWaveFunction>& wf_list,
                                      const RefVector<ParticleSet>& p_list,
                                      int iat,
                                      std::vector<GradType>& grad_now)
{
  const int num_wf = wf_list.size();
  grad_now.resize(num_wf);
  std::fill(grad_now.begin(), grad_now.end(), GradType(0));

  if (num_wf > 1)
  {
    ScopedTimer local_timer(wf_list[0].get().TWF_timers_[VGL_TIMER]);
    // Right now mw_evalGrad can only be called through an concrete instance of a wavefunctioncomponent
    const int num_wfc             = wf_list[0].get().Z.size();
    auto& wavefunction_components = wf_list[0].get().Z;

    std::vector<GradType> grad_now_z(num_wf);
    for (int i = 0, ii = VGL_TIMER; i < num_wfc; ++i, ii += TIMER_SKIP)
    {
      ScopedTimer localtimer(wf_list[0].get().WFC_timers_[ii]);
      const auto wfc_list(extractWFCRefList(wf_list, i));
      wavefunction_components[i]->mw_evalGrad(wfc_list, p_list, iat, grad_now_z);
      for (int iw = 0; iw < wf_list.size(); iw++)
        grad_now[iw] += grad_now_z[iw];
    }
  }
  else if (wf_list.size() == 1)
    grad_now[0] = wf_list[0].get().evalGrad(p_list[0], iat);
}


// Evaluates the gradient w.r.t. to the source of the Laplacian
// w.r.t. to the electrons of the wave function.
TrialWaveFunction::GradType TrialWaveFunction::evalGradSource(ParticleSet& P, ParticleSet& source, int iat)
{
  GradType grad_iat = GradType();
  for (int i = 0; i < Z.size(); ++i)
    grad_iat += Z[i]->evalGradSource(P, source, iat);
  return grad_iat;
}

TrialWaveFunction::GradType TrialWaveFunction::evalGradSource(
    ParticleSet& P,
    ParticleSet& source,
    int iat,
    TinyVector<ParticleSet::ParticleGradient_t, OHMMS_DIM>& grad_grad,
    TinyVector<ParticleSet::ParticleLaplacian_t, OHMMS_DIM>& lapl_grad)
{
  GradType grad_iat = GradType();
  for (int dim = 0; dim < OHMMS_DIM; dim++)
    for (int i = 0; i < grad_grad[0].size(); i++)
    {
      grad_grad[dim][i] = GradType();
      lapl_grad[dim][i] = 0.0;
    }
  for (int i = 0; i < Z.size(); ++i)
    grad_iat += Z[i]->evalGradSource(P, source, iat, grad_grad, lapl_grad);
  return grad_iat;
}

TrialWaveFunction::ValueType TrialWaveFunction::calcRatioGrad(ParticleSet& P, int iat, GradType& grad_iat)
{
  ScopedTimer local_timer(TWF_timers_[VGL_TIMER]);
  grad_iat = 0.0;
  PsiValueType r(1.0);
  if (use_tasking_)
  {
    std::vector<GradType> grad_components(Z.size(), GradType(0.0));
    std::vector<PsiValueType> ratio_components(Z.size(), 0.0);
    for (int i = 0, ii = VGL_TIMER; i < Z.size(); ++i, ii += TIMER_SKIP)
    {
      ScopedTimer z_timer(WFC_timers_[ii]);
      Z[i]->ratioGradAsync(P, iat, ratio_components[i], grad_components[i]);
    }

#pragma omp taskwait
    for (int i = 0; i < Z.size(); ++i)
    {
      grad_iat += grad_components[i];
      r *= ratio_components[i];
    }
  }
  else
    for (int i = 0, ii = VGL_TIMER; i < Z.size(); ++i, ii += TIMER_SKIP)
    {
      ScopedTimer z_timer(WFC_timers_[ii]);
      r *= Z[i]->ratioGrad(P, iat, grad_iat);
    }
  LogValueType logratio = convertValueToLog(r);
  PhaseDiff             = std::imag(logratio);
  return static_cast<ValueType>(r);
}

TrialWaveFunction::ValueType TrialWaveFunction::calcRatioGradWithSpin(ParticleSet& P,
                                                                      int iat,
                                                                      GradType& grad_iat,
                                                                      ComplexType& spingrad_iat)
{
  ScopedTimer local_timer(TWF_timers_[VGL_TIMER]);
  grad_iat     = 0.0;
  spingrad_iat = 0.0;
  PsiValueType r(1.0);
  for (int i = 0, ii = VGL_TIMER; i < Z.size(); ++i, ii += TIMER_SKIP)
  {
    ScopedTimer z_timer(WFC_timers_[ii]);
    r *= Z[i]->ratioGradWithSpin(P, iat, grad_iat, spingrad_iat);
  }

  LogValueType logratio = convertValueToLog(r);
  PhaseDiff             = std::imag(logratio);
  return static_cast<ValueType>(r);
}

void TrialWaveFunction::flex_calcRatioGrad(const RefVector<TrialWaveFunction>& wf_list,
                                           const RefVector<ParticleSet>& p_list,
                                           int iat,
                                           std::vector<PsiValueType>& ratios,
                                           std::vector<GradType>& grad_new)
{
  const int num_wf = wf_list.size();
  grad_new.resize(num_wf);
  std::fill(grad_new.begin(), grad_new.end(), GradType(0));
  ratios.resize(num_wf);
  std::fill(ratios.begin(), ratios.end(), PsiValueType(1));

  if (wf_list.size() > 1)
  {
    ScopedTimer local_timer(wf_list[0].get().TWF_timers_[VGL_TIMER]);
    const int num_wfc             = wf_list[0].get().Z.size();
    auto& wavefunction_components = wf_list[0].get().Z;

    if (wf_list[0].get().use_tasking_)
    {
      std::vector<std::vector<PsiValueType>> ratios_components(num_wfc, std::vector<PsiValueType>(wf_list.size()));
      std::vector<std::vector<GradType>> grads_components(num_wfc, std::vector<GradType>(wf_list.size()));
      for (int i = 0, ii = VGL_TIMER; i < num_wfc; ++i, ii += TIMER_SKIP)
      {
        ScopedTimer z_timer(wf_list[0].get().WFC_timers_[ii]);
        const auto wfc_list(extractWFCRefList(wf_list, i));
        wavefunction_components[i]->mw_ratioGradAsync(wfc_list, p_list, iat, ratios_components[i], grads_components[i]);
      }

#pragma omp taskwait
      for (int i = 0; i < num_wfc; ++i)
        for (int iw = 0; iw < wf_list.size(); iw++)
        {
          ratios[iw] *= ratios_components[i][iw];
          grad_new[iw] += grads_components[i][iw];
        }
    }
    else
    {
      std::vector<PsiValueType> ratios_z(wf_list.size());
      for (int i = 0, ii = VGL_TIMER; i < num_wfc; ++i, ii += TIMER_SKIP)
      {
        ScopedTimer z_timer(wf_list[0].get().WFC_timers_[ii]);
        const auto wfc_list(extractWFCRefList(wf_list, i));
        wavefunction_components[i]->mw_ratioGrad(wfc_list, p_list, iat, ratios_z, grad_new);
        for (int iw = 0; iw < wf_list.size(); iw++)
          ratios[iw] *= ratios_z[iw];
      }
    }
    for (int iw = 0; iw < wf_list.size(); iw++)
      wf_list[iw].get().PhaseDiff = std::imag(std::arg(ratios[iw]));
  }
  else if (wf_list.size() == 1)
    ratios[0] = wf_list[0].get().calcRatioGrad(p_list[0], iat, grad_new[0]);
}

void TrialWaveFunction::printGL(ParticleSet::ParticleGradient_t& G,
                                ParticleSet::ParticleLaplacian_t& L,
                                std::string tag)
{
  std::ostringstream o;
  o << "---  reporting " << tag << std::endl << "  ---" << std::endl;
  for (int iat = 0; iat < L.size(); iat++)
    o << "index: " << std::fixed << iat << std::scientific << "   G: " << G[iat][0] << "  " << G[iat][1] << "  "
      << G[iat][2] << "   L: " << L[iat] << std::endl;
  o << "---  end  ---" << std::endl;
  std::cout << o.str();
}

/** restore to the original state
 * @param iat index of the particle with a trial move
 *
 * The proposed move of the iath particle is rejected.
 * All the temporary data should be restored to the state prior to the move.
 */
void TrialWaveFunction::rejectMove(int iat)
{
  for (int i = 0; i < Z.size(); i++)
    Z[i]->restore(iat);
  PhaseDiff = 0;
}

/** update the state with the new data
 * @param P ParticleSet
 * @param iat index of the particle with a trial move
 *
 * The proposed move of the iath particle is accepted.
 * All the temporary data should be incorporated so that the next move is valid.
 */
void TrialWaveFunction::acceptMove(ParticleSet& P, int iat, bool safe_to_delay)
{
  ScopedTimer local_timer(TWF_timers_[ACCEPT_TIMER]);
  for (int i = 0, ii = ACCEPT_TIMER; i < Z.size(); i++, ii += TIMER_SKIP)
  {
    ScopedTimer z_timer(WFC_timers_[ii]);
    Z[i]->acceptMove(P, iat, safe_to_delay);
  }
  PhaseValue += PhaseDiff;
  PhaseDiff = 0.0;
  LogValue  = 0;
  for (int i = 0; i < Z.size(); i++)
    LogValue += std::real(Z[i]->LogValue);
}

void TrialWaveFunction::flex_accept_rejectMove(const RefVector<TrialWaveFunction>& wf_list,
                                               const RefVector<ParticleSet>& p_list,
                                               int iat,
                                               const std::vector<bool>& isAccepted,
                                               bool safe_to_delay)
{
  if (wf_list.size() > 1)
  {
    ScopedTimer local_timer(wf_list[0].get().TWF_timers_[ACCEPT_TIMER]);
    const int num_wfc             = wf_list[0].get().Z.size();
    auto& wavefunction_components = wf_list[0].get().Z;

    for (int iw = 0; iw < wf_list.size(); iw++)
      if (isAccepted[iw])
      {
        wf_list[iw].get().LogValue   = 0;
        wf_list[iw].get().PhaseValue = 0;
      }

    for (int i = 0, ii = ACCEPT_TIMER; i < num_wfc; i++, ii += TIMER_SKIP)
    {
      ScopedTimer z_timer(wf_list[0].get().WFC_timers_[ii]);
      const auto wfc_list(extractWFCRefList(wf_list, i));
      wavefunction_components[i]->mw_accept_rejectMove(wfc_list, p_list, iat, isAccepted, safe_to_delay);
      for (int iw = 0; iw < wf_list.size(); iw++)
        if (isAccepted[iw])
        {
          wf_list[iw].get().LogValue += std::real(wfc_list[iw].get().LogValue);
          wf_list[iw].get().PhaseValue += std::imag(wfc_list[iw].get().LogValue);
        }
    }
  }
  else if (wf_list.size() == 1)
  {
    if (isAccepted[0])
      wf_list[0].get().acceptMove(p_list[0], iat, safe_to_delay);
    else
      wf_list[0].get().rejectMove(iat);
  }
}

void TrialWaveFunction::completeUpdates()
{
  ScopedTimer local_timer(TWF_timers_[ACCEPT_TIMER]);
  for (int i = 0, ii = ACCEPT_TIMER; i < Z.size(); i++, ii += TIMER_SKIP)
  {
    ScopedTimer z_timer(WFC_timers_[ii]);
    Z[i]->completeUpdates();
  }
}

void TrialWaveFunction::flex_completeUpdates(const RefVector<TrialWaveFunction>& wf_list)
{
  if (wf_list.size() > 1)
  {
    ScopedTimer local_timer(wf_list[0].get().TWF_timers_[ACCEPT_TIMER]);
    const int num_wfc             = wf_list[0].get().Z.size();
    auto& wavefunction_components = wf_list[0].get().Z;

    for (int i = 0, ii = ACCEPT_TIMER; i < num_wfc; i++, ii += TIMER_SKIP)
    {
      ScopedTimer z_timer(wf_list[0].get().WFC_timers_[ii]);
      const auto wfc_list(extractWFCRefList(wf_list, i));
      wavefunction_components[i]->mw_completeUpdates(wfc_list);
    }
  }
  else if (wf_list.size() == 1)
    wf_list[0].get().completeUpdates();
}

TrialWaveFunction::LogValueType TrialWaveFunction::evaluateGL(ParticleSet& P, bool fromscratch)
{
  ScopedTimer local_timer(TWF_timers_[BUFFER_TIMER]);
  P.G = 0.0;
  P.L = 0.0;
  LogValueType logpsi(0.0);
  for (int i = 0, ii = BUFFER_TIMER; i < Z.size(); ++i, ii += TIMER_SKIP)
  {
    ScopedTimer z_timer(WFC_timers_[ii]);
    logpsi += Z[i]->evaluateGL(P, P.G, P.L, fromscratch);
  }

  LogValue   = std::real(logpsi);
  PhaseValue = std::imag(logpsi);
  return logpsi;
}

void TrialWaveFunction::flex_evaluateGL(const RefVector<TrialWaveFunction>& wf_list,
                                        const RefVector<ParticleSet>& p_list,
                                        bool fromscratch)
{
  if (wf_list.size() > 1)
  {
    ScopedTimer local_timer(wf_list[0].get().TWF_timers_[BUFFER_TIMER]);

    constexpr RealType czero(0);
    const auto g_list(TrialWaveFunction::extractGRefList(wf_list));
    const auto l_list(TrialWaveFunction::extractLRefList(wf_list));

    const int num_particles = p_list[0].get().getTotalNum();
    for (TrialWaveFunction& wfs : wf_list)
    {
      wfs.G.resize(num_particles);
      wfs.L.resize(num_particles);
      wfs.G          = czero;
      wfs.L          = czero;
      wfs.LogValue   = czero;
      wfs.PhaseValue = czero;
    }

    auto& wavefunction_components = wf_list[0].get().Z;
    const int num_wfc             = wavefunction_components.size();

    for (int i = 0, ii = BUFFER_TIMER; i < num_wfc; ++i, ii += TIMER_SKIP)
    {
      ScopedTimer z_timer(wf_list[0].get().WFC_timers_[ii]);
      const auto wfc_list(extractWFCRefList(wf_list, i));
      wavefunction_components[i]->mw_evaluateGL(wfc_list, p_list, g_list, l_list, fromscratch);
      for (int iw = 0; iw < wf_list.size(); iw++)
      {
        wf_list[iw].get().LogValue   += std::real(wfc_list[iw].get().LogValue);
        wf_list[iw].get().PhaseValue += std::imag(wfc_list[iw].get().LogValue);
      }
    }
    auto copyToP = [](ParticleSet& pset, TrialWaveFunction& twf) {
      pset.G = twf.G;
      pset.L = twf.L;
    };
    // Ye: temporal workaround to have P.G/L always defined.
    // remove when KineticEnergy use WF.G/L instead of P.G/L
    for (int iw = 0; iw < wf_list.size(); iw++)
      copyToP(p_list[iw], wf_list[iw]);
  }
  else if (wf_list.size() == 1)
  {
    wf_list[0].get().evaluateGL(p_list[0], fromscratch);
    // Ye: temporal workaround to have WF.G/L always defined.
    // remove when KineticEnergy use WF.G/L instead of P.G/L
    wf_list[0].get().G = p_list[0].get().G;
    wf_list[0].get().L = p_list[0].get().L;
  }
}


void TrialWaveFunction::checkInVariables(opt_variables_type& active)
{
  for (int i = 0; i < Z.size(); i++)
    Z[i]->checkInVariables(active);
}

void TrialWaveFunction::checkOutVariables(const opt_variables_type& active)
{
  for (int i = 0; i < Z.size(); i++)
    Z[i]->checkOutVariables(active);
}

void TrialWaveFunction::resetParameters(const opt_variables_type& active)
{
  for (int i = 0; i < Z.size(); i++)
    Z[i]->resetParameters(active);
}

void TrialWaveFunction::reportStatus(std::ostream& os)
{
  for (int i = 0; i < Z.size(); i++)
    Z[i]->reportStatus(os);
}

void TrialWaveFunction::getLogs(std::vector<RealType>& lvals)
{
  lvals.resize(Z.size(), 0);
  for (int i = 0; i < Z.size(); i++)
  {
    lvals[i] = std::real(Z[i]->LogValue);
  }
}

void TrialWaveFunction::getPhases(std::vector<RealType>& pvals)
{
  pvals.resize(Z.size(), 0);
  for (int i = 0; i < Z.size(); i++)
  {
    pvals[i] = std::imag(Z[i]->LogValue);
  }
}

void TrialWaveFunction::registerData(ParticleSet& P, WFBufferType& buf)
{
  ScopedTimer local_timer(TWF_timers_[BUFFER_TIMER]);
  //save the current position
  BufferCursor        = buf.current();
  BufferCursor_scalar = buf.current_scalar();
  for (int i = 0, ii = BUFFER_TIMER; i < Z.size(); ++i, ii += TIMER_SKIP)
  {
    ScopedTimer z_timer(WFC_timers_[ii]);
    Z[i]->registerData(P, buf);
  }
  buf.add(PhaseValue);
  buf.add(LogValue);
}

void TrialWaveFunction::flex_registerData(const UPtrVector<TrialWaveFunction>& wf_list,
                                          const UPtrVector<ParticleSet>& P_list,
                                          const RefVector<WFBufferType>& buf_list)
{
  if (wf_list.size() == 0)
    return;
  ScopedTimer local_timer(wf_list[0]->TWF_timers_[BUFFER_TIMER]);
  auto setBufferCursors = [](TrialWaveFunction& twf, WFBufferType& wb) {
    twf.BufferCursor        = wb.current();
    twf.BufferCursor_scalar = wb.current_scalar();
  };
  for (int iw = 0; iw < wf_list.size(); iw++)
  {
    setBufferCursors(*(wf_list[iw]), buf_list[iw]);
  }

  const int num_wfc             = wf_list[0]->Z.size();
  auto& wavefunction_components = wf_list[0]->Z;
  for (int i = 0, ii = BUFFER_TIMER; i < num_wfc; i++, ii += TIMER_SKIP)
  {
    ScopedTimer z_timer(wf_list[0]->WFC_timers_[ii]);
    std::vector<WaveFunctionComponent*> wfc_list(extractWFCPtrList(wf_list, i));

    wavefunction_components[i]->mw_registerData(wfc_list, convertUPtrToPtrVector(P_list), buf_list);
  }

  auto addPhaseAndLog = [](WFBufferType& wfb, TrialWaveFunction& twf) {
    wfb.add(twf.PhaseValue);
    wfb.add(twf.LogValue);
  };
  for (int iw = 0; iw < wf_list.size(); iw++)
    addPhaseAndLog(buf_list[iw], *(wf_list[iw]));
}

void TrialWaveFunction::debugOnlyCheckBuffer(WFBufferType& buffer)
{
#ifndef NDEBUG
  if (buffer.size() < buffer.current() + buffer.current_scalar() * sizeof(FullPrecRealType))
  {
    std::ostringstream assert_message;
    assert_message << "On thread:" << Concurrency::getWorkerId<>() << "  buf_list[iw].get().size():" << buffer.size()
                   << " < buf_list[iw].get().current():" << buffer.current()
                   << " + buf.current_scalar():" << buffer.current_scalar()
                   << " * sizeof(FullPrecRealType):" << sizeof(FullPrecRealType) << '\n';
    throw std::runtime_error(assert_message.str());
  }
#endif
}

TrialWaveFunction::RealType TrialWaveFunction::updateBuffer(ParticleSet& P, WFBufferType& buf, bool fromscratch)
{
  ScopedTimer local_timer(TWF_timers_[BUFFER_TIMER]);
  P.G = 0.0;
  P.L = 0.0;
  buf.rewind(BufferCursor, BufferCursor_scalar);
  LogValueType logpsi(0.0);
  for (int i = 0, ii = BUFFER_TIMER; i < Z.size(); ++i, ii += TIMER_SKIP)
  {
    ScopedTimer z_timer(WFC_timers_[ii]);
    logpsi += Z[i]->updateBuffer(P, buf, fromscratch);
  }

  LogValue   = std::real(logpsi);
  PhaseValue = std::imag(logpsi);
  //printGL(P.G,P.L);
  buf.put(PhaseValue);
  buf.put(LogValue);
  // Ye: temperal added check, to be removed
  debugOnlyCheckBuffer(buf);
  return LogValue;
}

/** updates "buffer" for multiple wavefunctions SIDEFFECT: reduces wfc values and phases to TWFs
 *
 *  NO UNIT TEST
 */
void TrialWaveFunction::flex_updateBuffer(const RefVector<TrialWaveFunction>& wf_list,
                                          const RefVector<ParticleSet>& p_list,
                                          const RefVector<WFBufferType>& buf_list,
                                          bool fromscratch)
{
  if (wf_list.size() == 0)
    return;
  ScopedTimer local_timer(wf_list[0].get().TWF_timers_[BUFFER_TIMER]);
  for (int iw = 0; iw < wf_list.size(); iw++)
  {
    constexpr RealType czero(0);

    p_list[iw].get().G           = czero; // Ye: remove when updateBuffer of all the WFC uses WF.G/L
    p_list[iw].get().L           = czero; // Ye: remove when updateBuffer of all the WFC uses WF.G/L
    wf_list[iw].get().LogValue   = czero;
    wf_list[iw].get().PhaseValue = czero;
    buf_list[iw].get().rewind(wf_list[iw].get().BufferCursor, wf_list[iw].get().BufferCursor_scalar);
  }

  auto& wavefunction_components = wf_list[0].get().Z;
  const int num_wfc             = wavefunction_components.size();

  for (int i = 0, ii = BUFFER_TIMER; i < num_wfc; ++i, ii += TIMER_SKIP)
  {
    ScopedTimer z_timer(wf_list[0].get().WFC_timers_[ii]);
    const auto wfc_list(extractWFCRefList(wf_list, i));
    wavefunction_components[i]->mw_updateBuffer(wfc_list, p_list, buf_list, fromscratch);
    for (int iw = 0; iw < wf_list.size(); iw++)
    {
      wf_list[iw].get().LogValue += std::real(wfc_list[iw].get().LogValue);
      wf_list[iw].get().PhaseValue += std::imag(wfc_list[iw].get().LogValue);
    }
  }

  for (int iw = 0; iw < wf_list.size(); iw++)
  {
    buf_list[iw].get().put(wf_list[iw].get().PhaseValue);
    buf_list[iw].get().put(wf_list[iw].get().LogValue);
    debugOnlyCheckBuffer(buf_list[iw]);
  }
}


void TrialWaveFunction::copyFromBuffer(ParticleSet& P, WFBufferType& buf)
{
  ScopedTimer local_timer(TWF_timers_[BUFFER_TIMER]);
  buf.rewind(BufferCursor, BufferCursor_scalar);
  for (int i = 0, ii = BUFFER_TIMER; i < Z.size(); ++i, ii += TIMER_SKIP)
  {
    ScopedTimer z_timer(WFC_timers_[ii]);
    Z[i]->copyFromBuffer(P, buf);
  }
  //get the gradients and laplacians from the buffer
  buf.get(PhaseValue);
  buf.get(LogValue);
  debugOnlyCheckBuffer(buf);
}

void TrialWaveFunction::flex_copyFromBuffer(const RefVector<TrialWaveFunction>& wf_list,
                                            const RefVector<ParticleSet>& p_list,
                                            const RefVector<WFBufferType>& buf_list) const
{
  if (wf_list.size() == 0)
    return;
  ScopedTimer local_timer(TWF_timers_[BUFFER_TIMER]);
  auto rewind = [](WFBufferType& buf, TrialWaveFunction& twf) {
    buf.rewind(twf.BufferCursor, twf.BufferCursor_scalar);
  };
  for (int iw = 0; iw < wf_list.size(); iw++)
    rewind(buf_list[iw], wf_list[iw]);

  for (int i = 0, ii = BUFFER_TIMER; i < Z.size(); ++i, ii += TIMER_SKIP)
  {
    ScopedTimer z_timer(WFC_timers_[ii]);
    const auto wfc_list(extractWFCRefList(wf_list, i));
    Z[i]->mw_copyFromBuffer(wfc_list, p_list, buf_list);
  }

  auto bufGetTwfLog = [](WFBufferType& buf, TrialWaveFunction& twf) {
    buf.get(twf.PhaseValue);
    buf.get(twf.LogValue);
    debugOnlyCheckBuffer(buf);
  };
  for (int iw = 0; iw < wf_list.size(); iw++)
    bufGetTwfLog(buf_list[iw], wf_list[iw]);
}

void TrialWaveFunction::evaluateRatios(const VirtualParticleSet& VP, std::vector<ValueType>& ratios, ComputeType ct)
{
  ScopedTimer local_timer(TWF_timers_[NL_TIMER]);
  assert(VP.getTotalNum() == ratios.size());
  std::vector<ValueType> t(ratios.size());
  std::fill(ratios.begin(), ratios.end(), 1.0);
  for (int i = 0, ii = NL_TIMER; i < Z.size(); ++i, ii += TIMER_SKIP)
    if (ct == ComputeType::ALL || (Z[i]->is_fermionic && ct == ComputeType::FERMIONIC) ||
        (!Z[i]->is_fermionic && ct == ComputeType::NONFERMIONIC))
    {
      ScopedTimer z_timer(WFC_timers_[ii]);
      Z[i]->evaluateRatios(VP, t);
      for (int j = 0; j < ratios.size(); ++j)
        ratios[j] *= t[j];
    }
}

void TrialWaveFunction::flex_evaluateRatios(const RefVector<TrialWaveFunction>& wf_list,
                                            const RefVector<const VirtualParticleSet>& vp_list,
                                            const RefVector<std::vector<ValueType>>& ratios_list,
                                            ComputeType ct)
{
  if (wf_list.size() > 1)
  {
    ScopedTimer local_timer(wf_list[0].get().TWF_timers_[NL_TIMER]);
    auto& wavefunction_components = Z;
    std::vector<std::vector<ValueType>> t(ratios_list.size());
    for (int iw = 0; iw < wf_list.size(); iw++)
    {
      std::vector<ValueType>& ratios = ratios_list[iw];
      assert(vp_list[iw].get().getTotalNum() == ratios.size());
      std::fill(ratios.begin(), ratios.end(), 1.0);
      t[iw].resize(ratios.size());
    }

    for (int i = 0, ii = NL_TIMER; i < wavefunction_components.size(); i++, ii += TIMER_SKIP)
      if (ct == ComputeType::ALL || (wavefunction_components[i]->is_fermionic && ct == ComputeType::FERMIONIC) ||
          (!wavefunction_components[i]->is_fermionic && ct == ComputeType::NONFERMIONIC))
      {
        ScopedTimer z_timer(wf_list[0].get().WFC_timers_[ii]);
        const auto wfc_list(extractWFCRefList(wf_list, i));
        wavefunction_components[i]->mw_evaluateRatios(wfc_list, vp_list, t);
        for (int iw = 0; iw < wf_list.size(); iw++)
        {
          std::vector<ValueType>& ratios = ratios_list[iw];
          for (int j = 0; j < ratios.size(); ++j)
            ratios[j] *= t[iw][j];
        }
      }
  }
  else if (wf_list.size() == 1)
    wf_list[0].get().evaluateRatios(vp_list[0], ratios_list[0], ct);
}

void TrialWaveFunction::evaluateDerivRatios(VirtualParticleSet& VP,
                                            const opt_variables_type& optvars,
                                            std::vector<ValueType>& ratios,
                                            Matrix<ValueType>& dratio)
{
#if defined(QMC_COMPLEX)
  APP_ABORT("TrialWaveFunction::evaluateDerivRatios not available for complex wavefunctions");
#else
  std::fill(ratios.begin(), ratios.end(), 1.0);
  std::vector<ValueType> t(ratios.size());
  for (int i = 0; i < Z.size(); ++i)
  {
    Z[i]->evaluateDerivRatios(VP, optvars, t, dratio);
    for (int j = 0; j < ratios.size(); ++j)
      ratios[j] *= t[j];
  }
#endif
}

bool TrialWaveFunction::put(xmlNodePtr cur) { return true; }

void TrialWaveFunction::reset() {}

TrialWaveFunction* TrialWaveFunction::makeClone(ParticleSet& tqp) const
{
  TrialWaveFunction* myclone   = new TrialWaveFunction(myName, use_tasking_, false);
  myclone->BufferCursor        = BufferCursor;
  myclone->BufferCursor_scalar = BufferCursor_scalar;
  for (int i = 0; i < Z.size(); ++i)
    myclone->addComponent(Z[i]->makeClone(tqp));
  myclone->OneOverM = OneOverM;
  return myclone;
}

/** evaluate derivatives of KE wrt optimizable varibles
 *
 * @todo WaveFunctionComponent objects should take the mass into account.
 */
void TrialWaveFunction::evaluateDerivatives(ParticleSet& P,
                                            const opt_variables_type& optvars,
                                            std::vector<ValueType>& dlogpsi,
                                            std::vector<ValueType>& dhpsioverpsi,
                                            bool project)
{
  //     // First, zero out derivatives
  //  This should only be done for some variables.
  //     for (int j=0; j<dlogpsi.size(); j++)
  //       dlogpsi[j] = dhpsioverpsi[j] = 0.0;
  for (int i = 0; i < Z.size(); i++)
  {
    if (Z[i]->dPsi)
      (Z[i]->dPsi)->evaluateDerivatives(P, optvars, dlogpsi, dhpsioverpsi);
    else
      Z[i]->evaluateDerivatives(P, optvars, dlogpsi, dhpsioverpsi);
  }
  //orbitals do not know about mass of particle.
  for (int i = 0; i < dhpsioverpsi.size(); i++)
    dhpsioverpsi[i] *= OneOverM;

  if (project)
  {
    for (int i = 0; i < Z.size(); i++)
    {
      if (Z[i]->dPsi)
        (Z[i]->dPsi)->multiplyDerivsByOrbR(dlogpsi);
      else
        Z[i]->multiplyDerivsByOrbR(dlogpsi);
    }
    RealType psiValue = std::exp(-LogValue) * std::cos(PhaseValue);
    for (int i = 0; i < dlogpsi.size(); i++)
      dlogpsi[i] *= psiValue;
  }
}

void TrialWaveFunction::flex_evaluateParameterDerivatives(const RefVector<TrialWaveFunction>& wf_list,
                                                          const RefVector<ParticleSet>& p_list,
                                                          const opt_variables_type& optvars,
                                                          RecordArray<ValueType>& dlogpsi,
                                                          RecordArray<ValueType>& dhpsioverpsi)
{
  int nparam = dlogpsi.nparam();
  for (int iw = 0; iw < wf_list.size(); iw++)
  {
    std::vector<ValueType> tmp_dlogpsi(nparam);
    std::vector<ValueType> tmp_dhpsioverpsi(nparam);
    TrialWaveFunction& twf = wf_list[iw];
    for (int i = 0; i < twf.Z.size(); i++)
    {
      if (twf.Z[i]->dPsi)
        (twf.Z[i]->dPsi)->evaluateDerivatives(p_list[iw], optvars, tmp_dlogpsi, tmp_dhpsioverpsi);
      else
        twf.Z[i]->evaluateDerivatives(p_list[iw], optvars, tmp_dlogpsi, tmp_dhpsioverpsi);
    }

    RealType OneOverM = twf.getReciprocalMass();
    for (int i = 0; i < nparam; i++)
    {
      dlogpsi.setValue(i, iw, tmp_dlogpsi[i]);
      //orbitals do not know about mass of particle.
      dhpsioverpsi.setValue(i, iw, tmp_dhpsioverpsi[i] * OneOverM);
    }
  }
}


void TrialWaveFunction::evaluateDerivativesWF(ParticleSet& P,
                                              const opt_variables_type& optvars,
                                              std::vector<ValueType>& dlogpsi)
{
  for (int i = 0; i < Z.size(); i++)
  {
    if (Z[i]->dPsi)
      (Z[i]->dPsi)->evaluateDerivativesWF(P, optvars, dlogpsi);
    else
      Z[i]->evaluateDerivativesWF(P, optvars, dlogpsi);
  }
}

void TrialWaveFunction::evaluateGradDerivatives(const ParticleSet::ParticleGradient_t& G_in,
                                                std::vector<ValueType>& dgradlogpsi)
{
  for (int i = 0; i < Z.size(); i++)
  {
    Z[i]->evaluateGradDerivatives(G_in, dgradlogpsi);
  }
}

TrialWaveFunction::RealType TrialWaveFunction::KECorrection() const
{
  RealType sum = 0.0;
  for (int i = 0; i < Z.size(); ++i)
    sum += Z[i]->KECorrection();
  return sum;
}

void TrialWaveFunction::evaluateRatiosAlltoOne(ParticleSet& P, std::vector<ValueType>& ratios)
{
  ScopedTimer local_timer(TWF_timers_[V_TIMER]);
  std::fill(ratios.begin(), ratios.end(), 1.0);
  std::vector<ValueType> t(ratios.size());
  for (int i = 0, ii = V_TIMER; i < Z.size(); ++i, ii += TIMER_SKIP)
  {
    ScopedTimer local_timer(WFC_timers_[ii]);
    Z[i]->evaluateRatiosAlltoOne(P, t);
    for (int j = 0; j < t.size(); ++j)
      ratios[j] *= t[j];
  }
}

void TrialWaveFunction::acquireResource(ResourceCollection& collection)
{
  for (int i = 0; i < Z.size(); ++i)
    Z[i]->acquireResource(collection);
}

void TrialWaveFunction::releaseResource(ResourceCollection& collection)
{
  for (int i = 0; i < Z.size(); ++i)
    Z[i]->releaseResource(collection);
}

RefVector<WaveFunctionComponent> TrialWaveFunction::extractWFCRefList(const RefVector<TrialWaveFunction>& wf_list,
                                                                      int id)
{
  std::vector<std::reference_wrapper<WaveFunctionComponent>> wfc_list;
  wfc_list.reserve(wf_list.size());
  for (TrialWaveFunction& wf : wf_list)
    wfc_list.push_back(*wf.Z[id]);
  return wfc_list;
}

std::vector<WaveFunctionComponent*> TrialWaveFunction::extractWFCPtrList(const UPtrVector<TrialWaveFunction>& WF_list,
                                                                         int id)
{
  std::vector<WaveFunctionComponent*> WFC_list;
  WFC_list.reserve(WF_list.size());
  for (auto& WF : WF_list)
    WFC_list.push_back(WF->Z[id]);
  return WFC_list;
}

RefVector<ParticleSet::ParticleGradient_t> TrialWaveFunction::extractGRefList(
    const RefVector<TrialWaveFunction>& wf_list)
{
  RefVector<ParticleSet::ParticleGradient_t> g_list;
  for (TrialWaveFunction& wf : wf_list)
    g_list.push_back(wf.G);
  return g_list;
}

RefVector<ParticleSet::ParticleLaplacian_t> TrialWaveFunction::extractLRefList(
    const RefVector<TrialWaveFunction>& wf_list)
{
  RefVector<ParticleSet::ParticleLaplacian_t> l_list;
  for (TrialWaveFunction& wf : wf_list)
    l_list.push_back(wf.L);
  return l_list;
}


} // namespace qmcplusplus
