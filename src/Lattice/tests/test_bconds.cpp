//////////////////////////////////////////////////////////////////////////////////////
// This file is distributed under the University of Illinois/NCSA Open Source License.
// See LICENSE file in top directory for details.
//
// Copyright (c) 2016 Jeongnim Kim and QMCPACK developers.
//
// File developed by:  Mark Dewing, markdewing@gmail.com, University of Illinois at Urbana-Champaign
//
// File created by: Mark Dewing, markdewing@gmail.com, University of Illinois at Urbana-Champaign
//////////////////////////////////////////////////////////////////////////////////////


#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include <stdio.h>
#include <string>

#include "OhmmsPETE/OhmmsMatrix.h"
#include "OhmmsPETE/TinyVector.h"
#include "Lattice/CrystalLattice.h"
#include "Lattice/ParticleBConds.h"
#include "Configuration.h"

using std::string;

namespace qmcplusplus
{
typedef TinyVector<double, 3> vec_t;

TEST_CASE("open_bconds", "[lattice]")
{
  CrystalLattice<OHMMS_PRECISION, OHMMS_DIM> Lattice;
  DTD_BConds<double, 3, SUPERCELL_OPEN> bcond(Lattice);

  vec_t v(3.0, 4.0, 5.0);

  double r2 = bcond.apply_bc(v);
  REQUIRE(Approx(r2) == 50.0);


  std::vector<vec_t> disps(1);
  disps[0] = v;
  std::vector<double> r(1), rinv(1), rr(1);

  bcond.apply_bc(disps, r, rinv);

  REQUIRE(Approx(r[0]) == std::sqrt(50.0));
  REQUIRE(Approx(rinv[0]) == 1.0 / std::sqrt(50.0));

  r[0] = 0.0;
  bcond.apply_bc(disps, r);
  REQUIRE(Approx(r[0]) == std::sqrt(50.0));

  bcond.evaluate_rsquared(disps.data(), rr.data(), disps.size());
  REQUIRE(Approx(rr[0]) == 50.0);
}

/** Lattice is defined but Open BC is also used.
 */
TEST_CASE("periodic_bulk_bconds", "[lattice]")
{
  CrystalLattice<OHMMS_PRECISION, OHMMS_DIM> Lattice;
  Lattice.BoxBConds = false; // Open BC
  Lattice.R.diagonal(0.4);
  Lattice.reset();

  REQUIRE(Lattice.Volume == Approx(0.4 * 0.4 * 0.4));

  DTD_BConds<double, 3, SUPERCELL_BULK> bcond(Lattice);

  vec_t v1(0.0, 0.0, 0.0);

  double r2 = bcond.apply_bc(v1);
  REQUIRE(r2 == 0.0);

  vec_t v2(0.5, 0.0, 0.0);
  r2 = bcond.apply_bc(v2);
  REQUIRE(r2 == Approx(0.01));

  vec_t v3(0.6, 1.2, -1.7);
  REQUIRE(Lattice.isValid(v3) == false);
  REQUIRE(Lattice.outOfBound(v3) == true);

  vec_t v4(0.45, 0.2, 0.1);
  REQUIRE(Lattice.isValid(v4) == true);
  REQUIRE(Lattice.outOfBound(v4) == false);
}

} // namespace qmcplusplus
