//////////////////////////////////////////////////////////////////
// (c) Copyright 2003-  by Jeongnim Kim
//////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////
//   National Center for Supercomputing Applications &
//   Materials Computation Center
//   University of Illinois, Urbana-Champaign
//   Urbana, IL 61801
//   e-mail: jnkim@ncsa.uiuc.edu
//
// Supported by
//   National Center for Supercomputing Applications, UIUC
//   Materials Computation Center, UIUC
//////////////////////////////////////////////////////////////////
// -*- C++ -*-
#include "QMCWaveFunctions/MultiSlaterDeterminant.h"

namespace qmcplusplus {

  MultiSlaterDeterminant::MultiSlaterDeterminant() { Optimizable=false;}
  MultiSlaterDeterminant::~MultiSlaterDeterminant() { }
  void MultiSlaterDeterminant::resetTargetParticleSet(ParticleSet& P) 
  {
    for(int i=0; i<SDets.size(); i++) SDets[i]->resetTargetParticleSet(P);
  }

  OrbitalBase::ValueType MultiSlaterDeterminant::evaluate(ParticleSet& P
      , ParticleSet::ParticleGradient_t& G, ParticleSet::ParticleLaplacian_t& L)
  { 
    int n = P.getTotalNum();
    ParticleSet::ParticleGradient_t g(n), gt(n);
    ParticleSet::ParticleLaplacian_t l(n), lt(n);
    ValueType psi = 0.0;
    for(int i=0; i<SDets.size(); i++){
      g=0.0;
      l=0.0;
      ValueType cdet = SDets[i]->evaluate(P,g,l);
      detValues[i] = cdet;
      cdet *= C[i];
      psi += cdet;
      gt += cdet*g;
      lt += cdet*l;
    }
    ValueType psiinv = 1.0/psi;
    G += gt*psiinv;
    L += lt*psiinv;
    return psi;
  }

  OrbitalBase::RealType MultiSlaterDeterminant::evaluateLog(ParticleSet& P
      , ParticleSet::ParticleGradient_t& G, ParticleSet::ParticleLaplacian_t& L)
  {
    //TO JEREMY: implement using evaluateLog for each slater determinant
    ValueType psi = evaluate(P,G,L);
    return LogValue = evaluateLogAndPhase(psi,PhaseValue);
  }

  OrbitalBase::GradType MultiSlaterDeterminant::evalGrad(ParticleSet& P, int iat)
  {
    DiracDeterminantBase::GradType g;
    for(int i=0; i<SDets.size(); i++){
      g += C[i]*detValues[i]*SDets[i]->evalGrad(P,iat);
    }
    ValueType psiinv = std::cos(PhaseValue)*std::exp(-1.0*LogValue);
    return g*psiinv;
//     APP_ABORT("IMPLEMENT MultiSlaterDeterminant::evalGrad");
//     return GradType();
  }

  OrbitalBase::ValueType MultiSlaterDeterminant::ratioGrad(ParticleSet& P
      , int iat, GradType& grad_iat)
  {
    DiracDeterminantBase::GradType g,gt;
    ValueType psiN = 0.0;
    for(int i=0; i<SDets.size(); i++){
      g=0;
      tempDetRatios[i] = SDets[i]->ratioGrad(P,iat,g);
      psiN += C[i]*detValues[i]*tempDetRatios[i];
      gt += C[i]*g*detValues[i]*tempDetRatios[i];
    }
    ValueType psiinv = std::cos(PhaseValue)*std::exp(-1.0*LogValue);
    grad_iat += gt/psiN;
    return psiN*psiinv;
//     APP_ABORT("IMPLEMENT MultiSlaterDeterminant::ratioGrad");
//     return 1.0;
  }

  OrbitalBase::ValueType  MultiSlaterDeterminant::ratio(ParticleSet& P, int iat
     , ParticleSet::ParticleGradient_t& dG,ParticleSet::ParticleLaplacian_t& dL)
  {
    int n = P.getTotalNum();
    ParticleSet::ParticleGradient_t g(n), gt(n);
    ParticleSet::ParticleLaplacian_t l(n), lt(n);
    ValueType psiN = 0.0;
    for(int i=0; i<SDets.size(); i++){ 
      g=0;
      l=0;
      tempDetRatios[i] = SDets[i]->ratio(P,iat,g,l);
      psiN += C[i]*detValues[i]*tempDetRatios[i];
      gt += C[i]*detValues[i]*tempDetRatios[i]*g;
      lt += C[i]*detValues[i]*tempDetRatios[i]*l;
    }
    ValueType psiinv = std::cos(PhaseValue)*std::exp(-1.0*LogValue);
    dG += gt/psiN;
    dL += lt/psiN;
    return psiN*psiinv;
//     APP_ABORT("IMPLEMENT MultiSlaterDeterminant::ratio");
//     return 1.0;
  }

  OrbitalBase::ValueType MultiSlaterDeterminant::ratio(ParticleSet& P, int iat)
  {
    APP_ABORT("JEREMY IMPLEMENT MultiSlaterDeterminant::ratio");
    return 1.0;
  }

  void MultiSlaterDeterminant::acceptMove(ParticleSet& P, int iat)
  {
    APP_ABORT("IMPLEMENT MultiSlaterDeterminant::acceptMove");
  }
  void MultiSlaterDeterminant::restore(int iat)
  {
    APP_ABORT("IMPLEMENT MultiSlaterDeterminant::restore");
  }

  void MultiSlaterDeterminant::update(ParticleSet& P
      , ParticleSet::ParticleGradient_t& dG, ParticleSet::ParticleLaplacian_t& dL
      , int iat)
  {
    APP_ABORT("IMPLEMENT MultiSlaterDeterminant::update");
  }

  OrbitalBase::RealType MultiSlaterDeterminant::evaluateLog(ParticleSet& P,BufferType& buf)
  {
    APP_ABORT("IMPLEMENT MultiSlaterDeterminant::evaluateLog");
    return 0.0;
  }

  OrbitalBase::RealType MultiSlaterDeterminant::registerData(ParticleSet& P, BufferType& buf)
  {
    APP_ABORT("IMPLEMENT MultiSlaterDeterminant::registerData");
    return 0.0;
  }

  OrbitalBase::RealType MultiSlaterDeterminant::updateBuffer(ParticleSet& P, BufferType& buf, bool fromscratch)
  {
    APP_ABORT("IMPLEMENT MultiSlaterDeterminant::updateBuffer");
    return 0.0;
  }

  void MultiSlaterDeterminant::copyFromBuffer(ParticleSet& P, BufferType& buf)
  {
    APP_ABORT("IMPLEMENT MultiSlaterDeterminant::copyFromBuffer");
  }


  /**
     add a new SlaterDeterminant with coefficient c to the 
     list of determinants
     Do not make it optimizable.
  */
  void MultiSlaterDeterminant::add(DeterminantSet_t* sdet, RealType c) 
  {
    SDets.push_back(sdet);
    C.push_back(c);
    detValues.push_back(0.0);
    tempDetRatios.push_back(0.0);
  }

  void MultiSlaterDeterminant::add(DeterminantSet_t* sdet, RealType c, const string& id)
  {
    SDets.push_back(sdet);
    C.push_back(c);
    detValues.push_back(0.0);
    tempDetRatios.push_back(0.0);
    myVars.insert(id,c,true,LINEAR_P);
  }

  void MultiSlaterDeterminant::checkInVariables(opt_variables_type& active)
  {
    if(Optimizable) 
    {
      if(myVars.size()) 
        active.insertFrom(myVars);
      else  
        Optimizable=false;
    }
  }

  void MultiSlaterDeterminant::checkOutVariables(const opt_variables_type& active)
  {
    if(Optimizable) myVars.getIndex(active);
  }

  /** resetParameters with optVariables
   *
   * USE_resetParameters
   */
  void MultiSlaterDeterminant::resetParameters(const opt_variables_type& active)
  {  
    if(Optimizable) 
    {
      for(int i=0; i<C.size(); i++) 
      {
        int loc=myVars.where(i);
        if(loc>=0) C[i]=myVars[i]=active[loc];
      }
      for(int i=0; i<SDets.size(); i++) SDets[i]->resetParameters(active);
    }
  }
  void MultiSlaterDeterminant::reportStatus(ostream& os)
  {
  }

  OrbitalBasePtr MultiSlaterDeterminant::makeClone(ParticleSet& tqp) const
  {
    APP_ABORT("IMPLEMENT OrbitalBase::makeClone");
    return 0;
  }

  void MultiSlaterDeterminant::evaluateDerivatives(ParticleSet& P, 
      const opt_variables_type& optvars,
      vector<RealType>& dlogpsi,
      vector<RealType>& dhpsioverpsi)
  {
    APP_ABORT("JEREMY IMPLEMENT OrbitalBase::evaluateDerivatives");
  }
}
/***************************************************************************
 * $RCSfile$   $Author: jnkim $
 * $Revision: 3416 $   $Date: 2008-12-07 11:34:49 -0600 (Sun, 07 Dec 2008) $
 * $Id: MultiSlaterDeterminant.cpp 3416 2008-12-07 17:34:49Z jnkim $
 ***************************************************************************/
