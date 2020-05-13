#include <cstdio>
#include <vector>

#include "thrust_wrapper.hpp"
#include "device_matrix.hpp"
#include "multi_array.hpp"

#ifdef __CUDACC__
#include <curand_kernel.h>
#else
#include <Random123/philox.h>

typedef r123::Philox2x64 RNG;
#endif

#ifdef __CUDACC__
#define DEVICE_FUNC __host__ __device__
#else
#define DEVICE_FUNC
#endif


/** \file
 *  This file contains computations required for Stokesian Dynamics, namely
 *  the particle's translational and angular velocities are computed from the 
 *  particle's positions and radii and the forces and torques that are acting
 *  on the particles.
 *
 *  In its current implementation, the neccessary subroutines are implemented
 *  as functors. The main functor is called solver, which wraps up the
 *  functionality of all the other functors in this file. For details, please
 *  see the description of the solver functor.
 *
 *  For details on the thermalization, please see the description of the
 *  thermalizer functor.
 *
 *  To understand the Stokesian Dynamics method, it is recommended to read
 *  @cite durlofsky87a, because it describes the method that is
 *  implemented here, apart from the thermalization.
 *
 *  All references to formulae in this file refer to the paper 
 *  @cite durlofsky87a, unless otherwise noted.
 *
 *  As they state in the paragraph below equation (2.17) it may be useful to
 *  also read @cite jeffrey84a to better understand the notation used to
 *  compute the grand mobility matrix in this Stokesian Dynamics method.
 *
 *  Both the F-T-version and the F-T-S-version have been implemented and can be
 *  selected via the FTS flag.
 *
 *  The general Stokesian Dynamics method allows to impose an external shear
 *  flow on the system. This has not been included in the ESPResSo interface,
 *  but could easily be included by passing the shear flow tensor to the SD
 *  routine and some almost trivial additional initialization.
 *
 *  Also, the figures in @cite cortez15a might help with an intuitive
 *  understanding of dipole, stokeslet and rotlet flow (missing out only on
 *  the stresslet flow).
 *
 */


namespace sd {

enum flags {
    NONE = 0,
    SELF_MOBILITY = 1 << 0,
    PAIR_MOBILITY = 1 << 1,
    LUBRICATION = 1 << 2,
    FTS = 1 << 3,
};

// Compute distance between particle pairs and check for overlaps
template <typename Policy, typename T>
struct check_dist {
    device_vector_view<T, Policy> const x;
    device_vector_view<T, Policy> const a;
    device_matrix_view<T, Policy> pd;
    device_matrix_view<std::size_t, Policy> const part_id;
    DEVICE_FUNC void operator()(std::size_t i) {
        std::size_t k = 6 * part_id(0, i);
        std::size_t j = 6 * part_id(1, i);

        T dx = x(j + 0) - x(k + 0);
        T dy = x(j + 1) - x(k + 1);
        T dz = x(j + 2) - x(k + 2);
        T dr = std::sqrt(dx * dx + dy * dy + dz * dz);
        T dr_inv = 1 / dr;

        if (dr <= a(part_id(0, i)) + a(part_id(1, i))) {
            //printf("Particles %lu and %lu overlapped! (distance %f < %f)\n",
            //       part_id(0, i), part_id(1, i), dr,
            //       a(part_id(0, i)) + a(part_id(1, i)));
            dr = NAN;
        }

        pd(0, i) = dx * dr_inv;
        pd(1, i) = dy * dr_inv;
        pd(2, i) = dz * dr_inv;
        pd(3, i) = dr;
    }
};




// The expression for the mobility matrix is given in Appendix A of
//
//   Durlofsky, L. and Brady, J.F. and Bossis G., J . Fluid Mech. 180, 21-49
//   (1987) https://authors.library.caltech.edu/32068/1/DURjfm87.pdf
//
// The submatrices mob_a, mob_b, mob_c, mob_gt, mob_ht, mob_m are
// given by equation (A 2).
//
// Alternatively, the same expressions are given in
//
//   Kim, S. and Mifflin, R.T., Physics of Fluids 28, 2033 (1985)
//   https://doi.org/10.1063/1.865384





/** The mobility functors fill in the elements of the grand mobility matrix.
 *  The goal is to have a numerical representation of the grand mobility
 *  matrix that can be found in equations (A 1) to (A 3).
 *  There are two mobility functors. One with isself=true and one with
 *  isself=false. They are described below.
 *
 *  To achieve this goal, all the sub-tensors a_mn, b_mn, c_mn, g_mn, h_mn and
 *  m_mn have to be computed. (m and n are used here instead of alpha and
 *  beta). The subscript indices denote single particles.
 *  This is done using the detailed description of the grand mobility matrix
 *  in equation (A 2).
 *
 *  In this source file, those sub-tensors are merged into larger sub-tensors
 *  of the grand mobility matrix. They are called mob_a, mob_b, mob_c, mob_gt,
 *  mob_ht and mob_m, respectively. (The blocks of the grand mobility matrix in
 *  (A 1) with common letters)
 *
 *  Those blocks are further merged. mob_a, mob_b and its mirror, and mob_c
 *  are merged into zmuf in the source code. mob_gt and mob_ht are merged into
 *  zmus. And mob_m is represented by zmes. To make that clear, compare
 *  equations (A 1) and (2.17). (Not entirely correct!! Blocks are organized
 *  in (UF) Blocks per particle)
 *
 *  The functor with isself=true computes the self-contributions of the grand
 *  mobility matrix, i.e. the sub-tensors a_mn, c_mn and m_mn that lie on the
 *  diagonal. For those sub-tensors, x_11 and y_11 from equation (A 3) have to
 *  be plugged into equation (A 2) (choose the one with matching superscript).
 *
 *  The functor with isself=false computes all the other contributions. Because
 *  there is always two particles involved for each contribution, they depend
 *  on the distance between the particle centers. For these expressions, x_12
 *  and y_12 from equation (A 3) have to be plugged into equation (A 2).
 */
template <typename Policy, typename T, bool isself>
struct mobility;

template <typename Policy, typename T>
struct mobility<Policy, T, true> {
    device_matrix_view<T, Policy> zmuf, zmus, zmes;
    device_vector_view<T, Policy> const a;
    T const eta;
    int const flg;

    // Determine the self contribution
    // This is independent of dr_inv, dx, dy, dz
    // (that is, the distance between particle centers)
    DEVICE_FUNC void operator()(std::size_t part_id) {
        // Fill the self mobility terms 
        // These are the sub-tensors of the grand mobility matrix as shown
        // in equation (A 1) that are located on the diagonal.
        // This functor fills in the sub-tensors for one single particle and
        // has to be executed for each particle.

        // mob_a, mob_c and mob_m are templates for those sub-tensors that just
        // have to be rescaled by a constant factor.
        static constexpr multi_array<T, 3, 3> const mob_a = {
            // clang-format off
            1, 0, 0,
            0, 1, 0,
            0, 0, 1
            // clang-format on
        };
        static constexpr multi_array<T, 3, 3> const mob_c = {
            // clang-format off
            3./4., 0    , 0    ,
            0    , 3./4., 0    ,
            0    , 0    , 3./4.
            // clang-format on
        };
        static constexpr multi_array<T, 5, 5> const mob_m = {
            // clang-format off
            9./5. , 0    , 0    , 0    , 9./10.,
            0     , 9./5., 0    , 0    , 0     ,
            0     , 0    , 9./5., 0    , 0     ,
            0     , 0    , 0    , 9./5., 0     ,
            9./10., 0    , 0    , 0    , 9./5.
            // clang-format on
        };


        // Compute where the self mobility submatrices of the current particle
        // are located in the grand mobility matrix.
        // For velocities/forces, there are 6 independent components.
        // (3 translation and 3 rotation)
        // For shear rate/stresslets there are 5 independent components.
        std::size_t const ph1 = 6 * part_id;
        std::size_t const ph2 = ph1 + 3;
        std::size_t const ph3 = 5 * part_id;

        // These are the non-dimensionalizations as stated in the paragraph
        // below equation (A 1).
        auto const visc1 = T{M_1_PI / 6. / eta / a(part_id)}; 
        auto const visc2 = T{visc1 / a(part_id)};
        auto const visc3 = T{visc2 / a(part_id)};

        // Now put the entries into the grand mobility matrix.
        for (std::size_t i = 0; i < 3; ++i) {
            zmuf(ph1 + i, ph1 + i) = visc1 * mob_a(i, i);
            zmuf(ph2 + i, ph2 + i) = visc3 * mob_c(i, i);
        }

        for (std::size_t i = 0; i < 5; ++i) {
            for (std::size_t j = 0; j < 5; ++j) {
                zmes(ph3 + i, ph3 + j) = visc3 * mob_m(i, j);
            }
        }
    }
};

template <typename Policy, typename T>
struct mobility<Policy, T, false> {
    device_matrix_view<T, Policy> zmuf, zmus, zmes;
    device_matrix_view<T, Policy> const pd;
    device_matrix_view<std::size_t, Policy> const part_id;
    device_vector_view<T, Policy> const a;
    T const eta;
    int const flg;

    // Determine the pair contribution
    DEVICE_FUNC void operator()(std::size_t pair_id) {
        // Kronecker-Delta
        static constexpr multi_array<T, 3, 3> const delta = {
            // clang-format off
            1, 0, 0,
            0, 1, 0,
            0, 0, 1
            // clang-format on
        };

        // Levi-Civita tensor
        static constexpr multi_array<T, 3, 3, 3> const eps = {
            // clang-format off
            0, 0, 0,   0, 0, 1,   0,-1, 0,
            0, 0,-1,   0, 0, 0,   1, 0, 0,
            0, 1, 0,  -1, 0, 0,   0, 0, 0
            // clang-format on
        };

        // Lookup table for the linearization of the shear rate tensor E and
        // the stresslet tensor S in equation (A 1).
        static constexpr multi_array<std::size_t, 2, 5> const mesid = {
            // clang-format off
            0, 0, 0, 1, 1,
            2, 1, 2, 2, 2
            // clang-format on
        };
        
        // particle ids of the involved particles
        std::size_t ph1 = part_id(0, pair_id);
        std::size_t ph2 = part_id(1, pair_id);
        // These are the non-dimensionalizations as stated in the paragraph
        // below equation (A 1).
        // However, modified, so that the case with two unequal spheres is
        // covered.
        T a12 = T{.5} * (a(ph1) + a(ph2));
        auto const visc1 = T{M_1_PI / 6. / eta / a12};
        auto const visc2 = T{visc1 / a12};
        auto const visc3 = T{visc2 / a12};

        // Components of unit vector along particle connection line as
        // described in paragraph below equation (A 1).
        T dx = pd(0, pair_id);
        T dy = pd(1, pair_id);
        T dz = pd(2, pair_id);
        // Non-dimensionalized inverted distance between particles 1/r
        T dr_inv = a12 / pd(3, pair_id);

        // Combine components into unit vector along particle connection line
        multi_array<T, 3> e = {dx, dy, dz};
        
        // This creates a lookup-table for the many e_i * e_j like 
        // multiplications in equation (A 2). 
        auto ee = outer(e, e);

        // Several powers of inverted inter-particle distance
        T dr_inv2 = dr_inv * dr_inv;
        T dr_inv3 = dr_inv2 * dr_inv;
        T dr_inv4 = dr_inv3 * dr_inv;
        T dr_inv5 = dr_inv4 * dr_inv;

        // The following scalar mobility functions can be found in
        // equation (A 3).
        T x12a = T{3. / 2.} * dr_inv - dr_inv3;
        T y12a = T{3. / 4.} * dr_inv + T{1. / 2.} * dr_inv3;

        T y12b = T{-3. / 4.} * dr_inv2;

        T x12c = T{3. / 4.} * dr_inv3;
        T y12c = T{-3. / 8.} * dr_inv3;

        T x12g = T{9. / 4.} * dr_inv2 - T{18. / 5.} * dr_inv4;
        T y12g = T{6. / 5.} * dr_inv4;                        

        T y12h = T{-1.0 * 9. / 8.} * dr_inv3;

        T x12m = T{-1.0 * 9. / 2.} * dr_inv3 + T{54. / 5.} * dr_inv5;
        T y12m = T{9. / 4.} * dr_inv3 - T{36. / 5.} * dr_inv5;
        T z12m = T{9. / 5.} * dr_inv5;

        // Equation (A 2) first, second, and third line
        multi_array<T, 3, 3> mob_a;
        multi_array<T, 3, 3> mob_b;
        multi_array<T, 3, 3> mob_c;
        for (std::size_t i = 0; i < 3; ++i) {
            for (std::size_t j = 0; j < 3; ++j) {
                // if i and j are different from one another, this returns the
                // "other" index missing in the set {0, 1, 2}
                std::size_t k = (3 - i - j) % 3; 

                mob_a(i, j) = x12a * ee(i, j) + y12a * (delta(i, j) - ee(i, j));
                mob_b(i, j) = y12b * eps(i, j, k) * e(k);
                mob_c(i, j) = x12c * ee(i, j) + y12c * (delta(i, j) - ee(i, j));
            }
        }

        // Equation (A 2) fourth and fifth line
        multi_array<T, 3, 3, 3> gt;
        multi_array<T, 3, 3, 3> ht;
        for (std::size_t k = 0; k < 3; ++k) {
            for (std::size_t i = 0; i < 3; ++i) {
                for (std::size_t j = 0; j < 3; ++j) {
                    gt(k, i, j) =
                        -(x12g * (ee(i, j) - T{1. / 3.} * delta(i, j)) * e(k) +
                          y12g * (e(i) * delta(j, k) + e(j) * delta(i, k) -
                                  T{2.0} * ee(i, j) * e(k)));

                    std::size_t l = (3 - j - k) % 3;
                    std::size_t m = (3 - i - k) % 3;
                    ht(k, i, j) = y12h * (ee(i, l) * eps(j, k, l) +
                                          ee(j, m) * eps(i, k, m));
                }
            }
        }

        // Equation (A 2) sixth line
        multi_array<T, 3, 3, 3, 3> m;
        for (std::size_t i = 0; i < 3; ++i) {
            for (std::size_t j = 0; j < 3; ++j) {
                for (std::size_t k = 0; k < 3; ++k) {
                    for (std::size_t l = 0; l < 3; ++l) {
                        m(i, j, k, l) =
                            T{3. / 2.} * x12m *
                                (ee(i, j) - T{1. / 3.} * delta(i, j)) *
                                (ee(k, l) - T{1. / 3.} * delta(k, l)) +
                            T{1. / 2.} * y12m *
                                (ee(i, k) * delta(j, l) +
                                 ee(j, k) * delta(i, l) +
                                 ee(i, l) * delta(j, k) +
                                 ee(j, l) * delta(i, k) -
                                 T{4.0} * ee(i, j) * ee(k, l)) +
                            T{1. / 2.} * z12m *
                                (delta(i, k) * delta(j, l) +
                                 delta(j, k) * delta(i, l) -
                                 delta(i, j) * delta(k, l) +
                                 ee(i, j) * delta(k, l) +
                                 ee(k, l) * delta(i, j) -
                                 ee(i, k) * delta(j, l) -
                                 ee(j, k) * delta(i, l) -
                                 ee(i, l) * delta(j, k) -
                                 ee(j, l) * delta(i, k) + ee(i, j) * ee(k, l));
                    }
                }
            }
        }

        // This segment of code converts the pair contributions to the grand
        // mobility tensor into a symmetric matrix.  Using the following
        // conversions of the shear rate E and the stresslet S into the vectors
        // EV and SV respectively. EV_1 = E_11 - E_33, EV_2 = 2 E_12, EV_3 = 2
        // E_13, EV_4 = 2 E_23, EV_5 = E_22 - E_33 SV_1 = S_11, SV_2 = S_12 =
        // S_21, SV_3 = S_13 = S_31, SV_4 = S_23 = S_32, SV_5 = S_22
        multi_array<T, 3, 5> mob_gt;
        multi_array<T, 3, 5> mob_ht;
        for (std::size_t i = 0; i < 3; ++i) {
            mob_gt(i, 0) = gt(i, 0, 0) - gt(i, 2, 2);
            mob_gt(i, 1) = T{2.0} * gt(i, 0, 1);
            mob_gt(i, 2) = T{2.0} * gt(i, 0, 2);
            mob_gt(i, 3) = T{2.0} * gt(i, 1, 2);
            mob_gt(i, 4) = gt(i, 1, 1) - gt(i, 2, 2);

            mob_ht(i, 0) = ht(i, 0, 0) - ht(i, 2, 2);
            mob_ht(i, 1) = T{2.0} * ht(i, 0, 1);
            mob_ht(i, 2) = T{2.0} * ht(i, 0, 2);
            mob_ht(i, 3) = T{2.0} * ht(i, 1, 2);
            mob_ht(i, 4) = ht(i, 1, 1) - ht(i, 2, 2);
        }

        multi_array<T, 5, 5> mob_m;
        for (std::size_t i = 0; i < 5; ++i) {
            if (i == 0 || i == 4) {
                mob_m(i, 0) = m(mesid(0, i), mesid(0, i), 0, 0) -
                              m(mesid(0, i), mesid(0, i), 2, 2) -
                              (m(mesid(1, i), mesid(1, i), 0, 0) -
                               m(mesid(1, i), mesid(1, i), 2, 2));
                mob_m(i, 1) = T{2.0} * (m(mesid(0, i), mesid(0, i), 0, 1) -
                                        m(mesid(1, i), mesid(1, i), 0, 1));
                mob_m(i, 2) = T{2.0} * (m(mesid(0, i), mesid(0, i), 0, 2) -
                                        m(mesid(1, i), mesid(1, i), 0, 2));
                mob_m(i, 3) = T{2.0} * (m(mesid(0, i), mesid(0, i), 1, 2) -
                                        m(mesid(1, i), mesid(1, i), 1, 2));
                mob_m(i, 4) = m(mesid(0, i), mesid(0, i), 1, 1) -
                              m(mesid(0, i), mesid(0, i), 2, 2) -
                              (m(mesid(1, i), mesid(1, i), 1, 1) -
                               m(mesid(1, i), mesid(1, i), 2, 2));
            } else {
                mob_m(i, 0) = T{2.0} * (m(mesid(0, i), mesid(1, i), 0, 0) -
                                        m(mesid(0, i), mesid(1, i), 2, 2));
                mob_m(i, 1) = T{4.0} * m(mesid(0, i), mesid(1, i), 0, 1);
                mob_m(i, 2) = T{4.0} * m(mesid(0, i), mesid(1, i), 0, 2);
                mob_m(i, 3) = T{4.0} * m(mesid(0, i), mesid(1, i), 1, 2);
                mob_m(i, 4) = T{2.0} * (m(mesid(0, i), mesid(1, i), 1, 1) -
                                        m(mesid(0, i), mesid(1, i), 2, 2));
            }
        }

        // Fill the pair mobility terms

        // Compute where the various submatrices of the current particle pair
        // are located in the grand mobility matrix.
        // For velocities/forces, there are 6 independent components.
        // (3 translation and 3 rotation)
        // For shear rate/stresslets there are 5 independent components.
        std::size_t ph5 = 5 * ph1;
        std::size_t ph6 = 5 * ph2;

        ph1 = 6 * ph1;
        ph2 = 6 * ph2;

        std::size_t ph3 = ph1 + 3;
        std::size_t ph4 = ph2 + 3;

        // Now copy values into the correct locations in the "big" matrix
        // and apply scaling
        for (std::size_t i = 0; i < 3; ++i) {
            for (std::size_t j = 0; j < 3; ++j) {
                zmuf(ph1 + i, ph2 + j) = visc1 * mob_a(i, j);
                zmuf(ph3 + i, ph2 + j) = visc2 * mob_b(i, j);
                zmuf(ph1 + i, ph4 + j) = 
                    -visc2 * mob_b(j, i); // mob_b transpose
                zmuf(ph3 + i, ph4 + j) = visc3 * mob_c(i, j);

                zmuf(ph2 + i, ph1 + j) = visc1 * mob_a(j, i);
                zmuf(ph4 + i, ph1 + j) = visc2 * mob_b(j, i);
                zmuf(ph2 + i, ph3 + j) =
                    -visc2 * mob_b(i, j); // mob_b transpose
                zmuf(ph4 + i, ph3 + j) = visc3 * mob_c(j, i);
            }

            for (std::size_t j = 0; j < 5; ++j) {
                // The paragraph under equation (A 1) claims that we would need
                // exponent n=3, but n=2 yields correct results.
                // Needs to be analytically verified.
                zmus(ph1 + i, ph6 + j) = visc2 * mob_gt(i, j);  
                zmus(ph2 + i, ph5 + j) = T{-1.0} * visc2 * mob_gt(i, j);  

                // We don't know whether this is the correct exponent.
                zmus(ph3 + i, ph6 + j) = visc3 * mob_ht(i, j);
                zmus(ph4 + i, ph5 + j) = visc3 * mob_ht(i, j);
            }
        }

        for (std::size_t i = 0; i < 5; ++i) {
            for (std::size_t j = 0; j < 5; ++j) {
                // We don't know whether this is the correct exponent.
                zmes(ph5 + i, ph6 + j) = visc3 * mob_m(i, j);
                zmes(ph6 + i, ph5 + j) = visc3 * mob_m(j, i);
            }
        }
    }
};



/** The lubrication functor adds pair wise interactions to the resistance
 *  matrix. This section of code has not been thoroughly commented yet.
 *  Therefore, compare to the part about lubrication, starting with the
 *  paragraph above equation (2.18).
 */
template <typename Policy, typename T>
struct lubrication {
    device_matrix_view<T, Policy> rfu, rfe, rse;
    device_matrix_view<T, Policy> const pd;
    device_matrix_view<std::size_t, Policy> const part_id;
    device_vector_view<T, Policy> const a;
    T const eta;
    int const flg;

    // Add the lubrication forces to the mobility inverse
    DEVICE_FUNC void operator()(std::size_t pair_id) {
        T dx = pd(0, pair_id);
        T dy = pd(1, pair_id);
        T dz = pd(2, pair_id);
        multi_array<T, 3> d = {dx, dy, dz};
        T dr = pd(3, pair_id);

        if (dr < 4.0) { //non-dimensionalize!
            std::size_t i = part_id(0, pair_id);
            std::size_t j = part_id(1, pair_id);

            std::size_t ira = i * 6;
            std::size_t irg = ira;
            std::size_t irm = i * 5;
            std::size_t icg = irm;

            std::size_t jca = j * 6;
            std::size_t jrg = jca;
            std::size_t jcm = j * 5;
            std::size_t jcg = jcm;

            multi_array<T, 12, 12> tabc;
            multi_array<T, 12, 10> tght;
            multi_array<T, 10, 10> tzm;
            calc_lub(pair_id, dr, d, tabc, tght, tzm);

            for (std::size_t jc = 0; jc < 6; ++jc) {
                std::size_t jl = jc + 6;
                std::size_t j1 = ira + jc;
                std::size_t j2 = jca + jc;

                for (std::size_t ir = 0; ir < jc + 1; ++ir) {
                    std::size_t il = ir + 6;
                    std::size_t i1 = ira + ir;
                    std::size_t i2 = jca + ir;

                    rfu(i1, j1) += tabc(ir, jc);
                    rfu(i2, j2) += tabc(il, jl);
                }
            }
            for (std::size_t jc = 6; jc < 12; ++jc) {
                std::size_t j1 = jca + jc - 6;

                for (std::size_t ir = 0; ir < 6; ++ir) {
                    std::size_t i1 = ira + ir;

                    rfu(i1, j1) += tabc(ir, jc);
                }
            }

            if (flg & FTS) {
                for (std::size_t jc = 0; jc < 5; ++jc) {
                    std::size_t jl = jc + 5;
                    std::size_t j1 = icg + jc;
                    std::size_t j2 = jcg + jc;

                    for (std::size_t ir = 0; ir < 6; ++ir) {
                        std::size_t il = ir + 6;
                        std::size_t i1 = irg + ir;
                        std::size_t i2 = jrg + ir;

                        rfe(i1, j1) += tght(ir, jc);
                        rfe(i2, j2) += tght(il, jl);
                        rfe(i1, j2) += tght(ir, jl);
                        rfe(i2, j1) += tght(il, jc);
                    }
                }
                for (std::size_t jc = 0; jc < 5; ++jc) {
                    std::size_t jl = jc + 5;
                    std::size_t j1 = irm + jc;
                    std::size_t j2 = jcm + jc;

                    for (std::size_t ir = 0; ir < jc + 1; ++ir) {
                        std::size_t il = ir + 5;
                        std::size_t i1 = irm + ir;
                        std::size_t i2 = jcm + ir;

                        rse(i1, j1) += tzm(ir, jc);
                        rse(i2, j2) += tzm(il, jl);
                    }
                }
                for (std::size_t jc = 5; jc < 10; ++jc) {
                    std::size_t j1 = jcm + jc - 5;

                    for (std::size_t ir = 0; ir < 5; ++ir) {
                        std::size_t i1 = irm + ir;

                        rse(i1, j1) += tzm(ir, jc);
                    }
                }
            }
        }
    }

    // computes the pair-wise lubrication interactions between particle pairs
    DEVICE_FUNC void calc_lub(size_t pair_id, double dr,
                              multi_array<T, 3> const &d,
                              multi_array<T, 12, 12> &tabc,
                              multi_array<T, 12, 10> &tght,
                              multi_array<T, 10, 10> &tzm) {
        static constexpr multi_array<T, 3, 3> const delta = {
            // clang-format off
            1, 0, 0,
            0, 1, 0,
            0, 0, 1
            // clang-format on
        };

        static constexpr multi_array<T, 3, 3, 3> const eps = {
            // clang-format off
            0, 0, 0,   0, 0, 1,   0,-1, 0,
            0, 0,-1,   0, 0, 0,   1, 0, 0,
            0, 1, 0,  -1, 0, 0,   0, 0, 0
            // clang-format on
        };

        static constexpr multi_array<std::size_t, 2, 5> const mesid = {
            // clang-format off
            0, 0, 0, 1, 1,
            2, 1, 2, 2, 2
            // clang-format on
        };

#include "lubrication_data.inl"

        std::size_t ph1 = part_id(0, pair_id);
        std::size_t ph2 = part_id(1, pair_id);

        T a11 = a(ph1);
        auto const visc11_1 = T{M_PI * 6. * eta * a11};
        auto const visc11_2 = T{visc11_1 * a11};
        auto const visc11_3 = T{visc11_2 * a11};

        T a22 = a(ph2);
        auto const visc22_1 = T{M_PI * 6. * eta * a22};
        auto const visc22_2 = T{visc22_1 * a22};
        auto const visc22_3 = T{visc22_2 * a22};

        T a12 = T{.5} * (a(ph1) + a(ph2));
        auto const visc12_1 = T{M_PI * 6. * eta * a12};
        auto const visc12_2 = T{visc12_1 * a12};
        auto const visc12_3 = T{visc12_2 * a12};

        double x11a, x12a, y11a, y12a, y11b, y12b, x11c, x12c, y11c, y12c, x11g,
            x12g, y11g, y12g, y11h, y12h, xm, ym, zm;
        if (dr <= 2.1) {
            double xi = dr - 2;

            double xi1 = 1 / xi;
            double dlx = std::log(xi1);

            double xdlx = xi * dlx;
            double dlx1 = dlx + xdlx;

            double csa1 = dlx * 1. / 6.;
            double csa2 = xdlx * 1. / 6.;
            double csa3 = dlx1 * 1. / 6.;
            double csa4 = 0.25 * xi1 + 0.225 * dlx;
            double csa5 = dlx * 1. / 15.;

            //*** a, btilda, and c terms for rfu.

            x11a = csa4 - 1.23041 + 3. / 112. * xdlx + 1.8918 * xi;
            x12a = -x11a + 0.00312 - 0.0011 * xi;
            y11a = csa1 - 0.39394 + 0.95665 * xi;
            y12a = -y11a + 0.00463606 - 0.007049 * xi;

            y11b = -csa1 + 0.408286 - xdlx * 1. / 12. - 0.84055 * xi;
            y12b = -y11b + 0.00230818 - 0.007508 * xi;

            x11c = 0.0479 - csa2 + 0.12494 * xi;
            x12c = -0.031031 + csa2 - 0.174476 * xi;
            y11c = 4. * csa5 - 0.605434 + 94. / 375. * xdlx + 0.939139 * xi;
            y12c = csa5 - 0.212032 + 31. / 375. * xdlx + 0.452843 * xi;

            //*** g and h terms for rsu.

            double csg1 = csa4 + 39. / 280. * xdlx;
            double csg2 = dlx * 1. / 12. + xdlx * 1. / 24.;

            x11g = csg1 - 1.16897 + 1.47882 * xi;
            x12g = -csg1 + 1.178967 - 1.480493 * xi;
            y11g = csg2 - 0.2041 + 0.442226 * xi;
            y12g = -csg2 + 0.216365 - 0.469830 * xi;

            y11h = 0.5 * csa5 - 0.143777 + 137. / 1500. * xdlx + 0.264207 * xi;
            y12h = 2. * csa5 - 0.298166 + 113. / 1500. * xdlx + 0.534123 * xi;

            //*** m term for rse.

            xm = 1. / 3. * xi1 + 0.3 * dlx - 1.48163 + 0.335714 * xdlx +
                 1.413604 * xi;
            ym = csa3 - 0.423489 + 0.827286 * xi;
            zm = 0.0129151 - 0.042284 * xi;

        } else {
            auto ida = static_cast<int>(20. * (dr - 2));
            int ib = -2 + ida;
            int ia = ib + 1;

            double c1 = (dr - rsabc[ib]) / (rsabc[ia] - rsabc[ib]);

            x11a = (x11as[ia] - x11as[ib]) * c1 + x11as[ib];
            x12a = (x12as[ia] - x12as[ib]) * c1 + x12as[ib];
            y11a = (y11as[ia] - y11as[ib]) * c1 + y11as[ib];
            y12a = (y12as[ia] - y12as[ib]) * c1 + y12as[ib];

            y11b = (y11bs[ia] - y11bs[ib]) * c1 + y11bs[ib];
            y12b = (y12bs[ia] - y12bs[ib]) * c1 + y12bs[ib];

            y11c = (y11cs[ia] - y11cs[ib]) * c1 + y11cs[ib];
            y12c = (y12cs[ia] - y12cs[ib]) * c1 + y12cs[ib];
            x11c = (x11cs[ia] - x11cs[ib]) * c1 + x11cs[ib];
            x12c = (x12cs[ia] - x12cs[ib]) * c1 + x12cs[ib];

            if (dr < 2.2) {
                ib = -10 + static_cast<int>(100. * (dr - 2));
            } else {
                ib = 6 + ida;
            }

            ia = ib + 1;

            double cgh = (dr - rsgh[ib]) / (rsgh[ia] - rsgh[ib]);

            x11g = (x11gs[ia] - x11gs[ib]) * cgh + x11gs[ib];
            x12g = (x12gs[ia] - x12gs[ib]) * cgh + x12gs[ib];
            y11g = (y11gs[ia] - y11gs[ib]) * cgh + y11gs[ib];
            y12g = (y12gs[ia] - y12gs[ib]) * cgh + y12gs[ib];

            y11h = (y11hs[ia] - y11hs[ib]) * cgh + y11hs[ib];
            y12h = (y12hs[ia] - y12hs[ib]) * cgh + y12hs[ib];

            double cm = (dr - rsm[ib]) / (rsm[ia] - rsm[ib]);

            xm = (xms[ia] - xms[ib]) * cm + xms[ib];
            ym = (yms[ia] - yms[ib]) * cm + yms[ib];
            zm = (zms[ia] - zms[ib]) * cm + zms[ib];
        }
        //***************************************************************************c
        //***************************************************************************c

        auto ee = outer(d, d);

        //***************************************************************************c
        //***************************************************************************c
        //****************************** form tabc for rfu
        //**************************c

        double xmy11a = x11a - y11a;
        double xmy12a = x12a - y12a;
        double xmy11c = x11c - y11c;
        double xmy12c = x12c - y12c;

        //*** insert upper half of a11.

        T tabc00 = xmy11a * ee(0, 0) + y11a;
        T tabc11 = xmy11a * ee(1, 1) + y11a;
        T tabc22 = xmy11a * ee(2, 2) + y11a;
        T tabc01 = xmy11a * ee(0, 1);
        T tabc02 = xmy11a * ee(0, 2);
        T tabc12 = xmy11a * ee(1, 2);
        tabc(0, 0) = visc11_1 * tabc00;
        tabc(1, 1) = visc11_1 * tabc11;
        tabc(2, 2) = visc11_1 * tabc22;
        tabc(0, 1) = visc11_1 * tabc01;
        tabc(0, 2) = visc11_1 * tabc02;
        tabc(1, 2) = visc11_1 * tabc12;

        //*** insert a12.

        tabc(0, 6) = visc12_1 * xmy12a * ee(0, 0) + y12a;
        tabc(1, 7) = visc12_1 * xmy12a * ee(1, 1) + y12a;
        tabc(2, 8) = visc12_1 * xmy12a * ee(2, 2) + y12a;
        tabc(0, 7) = visc12_1 * xmy12a * ee(0, 1);
        tabc(0, 8) = visc12_1 * xmy12a * ee(0, 2);
        tabc(1, 8) = visc12_1 * xmy12a * ee(1, 2);
        tabc(1, 6) = tabc(0, 7);
        tabc(2, 6) = tabc(0, 8);
        tabc(2, 7) = tabc(1, 8);

        //*** insert upper half of c11.

        T tabc33 = xmy11c * ee(0, 0) + y11c;
        T tabc44 = xmy11c * ee(1, 1) + y11c;
        T tabc55 = xmy11c * ee(2, 2) + y11c;
        T tabc34 = xmy11c * ee(0, 1);
        T tabc35 = xmy11c * ee(0, 2);
        T tabc45 = xmy11c * ee(1, 2);
        tabc(3, 3) = visc11_3 * tabc33;
        tabc(4, 4) = visc11_3 * tabc44;
        tabc(5, 5) = visc11_3 * tabc55;
        tabc(3, 4) = visc11_3 * tabc34;
        tabc(3, 5) = visc11_3 * tabc35;
        tabc(4, 5) = visc11_3 * tabc45;

        //*** insert c12.

        tabc(3, 9) = visc12_3 * xmy12c * ee(0, 0) + y12c;
        tabc(4, 10) = visc12_3 * xmy12c * ee(1, 1) + y12c;
        tabc(5, 11) = visc12_3 * xmy12c * ee(2, 2) + y12c;
        tabc(3, 10) = visc12_3 * xmy12c * ee(0, 1);
        tabc(3, 11) = visc12_3 * xmy12c * ee(0, 2);
        tabc(4, 11) = visc12_3 * xmy12c * ee(1, 2);
        tabc(4, 9) = tabc(3, 10);
        tabc(5, 9) = tabc(3, 11);
        tabc(5, 10) = tabc(4, 11);

        //*** fill in upper half of a22 (=a11).

        tabc(6, 6) = visc22_1 * tabc00;
        tabc(6, 7) = visc22_1 * tabc01;
        tabc(6, 8) = visc22_1 * tabc02;
        tabc(7, 7) = visc22_1 * tabc11;
        tabc(7, 8) = visc22_1 * tabc12;
        tabc(8, 8) = visc22_1 * tabc22;

        //*** fill in upper half of c22 (=c11).

        tabc(9, 9) = visc22_3 * tabc33;
        tabc(9, 10) = visc22_3 * tabc34;
        tabc(9, 11) = visc22_3 * tabc35;
        tabc(10, 10) = visc22_3 * tabc44;
        tabc(10, 11) = visc22_3 * tabc45;
        tabc(11, 11) = visc22_3 * tabc55;

        //*** insert bt11.

        tabc(0, 3) = 0.0;
        tabc(0, 4) = -visc11_2 * y11b * d(2);
        tabc(0, 5) = visc11_2 * y11b * d(1);
        tabc(1, 4) = 0.0;
        tabc(1, 5) = -visc11_2 * y11b * d(0);
        tabc(1, 3) = -tabc(0, 4);
        tabc(2, 3) = -tabc(0, 5);
        tabc(2, 4) = -tabc(1, 5);
        tabc(2, 5) = 0.0;

        //*** insert bt12.

        tabc(0, 9) = 0.0;
        tabc(0, 10) = visc12_2 * y12b * d(2);
        tabc(0, 11) = -visc12_2 * y12b * d(1);
        tabc(1, 10) = 0.0;
        tabc(1, 11) = visc12_2 * y12b * d(0);
        tabc(1, 9) = -tabc(0, 10);
        tabc(2, 9) = -tabc(0, 11);
        tabc(2, 10) = -tabc(1, 11);
        tabc(2, 11) = 0.0;

        //***************************************************************************c
        //***************************************************************************c
        //*** fill in bt22 (=-bt11) and b12 (=bt12).

        for (std::size_t j3 = 3; j3 < 6; ++j3) {
            std::size_t j6 = j3 + 3;
            std::size_t j9 = j3 + 6;

            for (std::size_t i = 0; i < 3; ++i) {
                std::size_t i3 = i + 3;
                std::size_t i6 = i + 6;

                tabc(i3, j6) = tabc(i, j9);
                tabc(i6, j9) = -tabc(i, j3);
            }
        }

        if (!(flg & FTS)) {
            return;
        }

        //***************************************************************************c
        //***************************************************************************c
        //****************************** form tght for rfe
        //**************************c
        //*** insert gt11.
        multi_array<T, 3, 3, 3> gt;
        multi_array<T, 3, 3, 3> ht;
        for (std::size_t k = 0; k < 3; ++k) {
            for (std::size_t i = 0; i < 3;
                 ++i) { // TODO: Is the 3 actually correct?  In the Fortran
                        // version it's 2
                for (std::size_t j = 0; j < 3; ++j) {
                    gt(k, i, j) =
                        x11g * (ee(i, j) - T{1. / 3.} * delta(i, j)) * d(k) +
                        y11g * (d(i) * delta(j, k) + d(j) * delta(i, k) -
                                T{2.0} * ee(i, j) * d(k));

                    std::size_t l = (3 - j - k) % 3;
                    std::size_t m = (3 - i - k) % 3;
                    ht(k, i, j) = y11h * (ee(i, l) * eps(j, k, l) +
                                          ee(j, m) * eps(i, k, m));
                }
            }
        }

        for (std::size_t i = 0; i < 3; ++i) {
            std::size_t k = i + 6;
            std::size_t j = i + 3;

            tght(k, 0) = visc11_3 * gt(i, 0, 0) - gt(i, 2, 2);
            tght(k, 1) = visc11_3 * 2.0 * gt(i, 0, 1);
            tght(k, 2) = visc11_3 * 2.0 * gt(i, 0, 2);
            tght(k, 3) = visc11_3 * 2.0 * gt(i, 1, 2);
            tght(k, 4) = visc11_3 * gt(i, 1, 1) - gt(i, 2, 2);

            tght(j, 5) = visc11_3 * ht(i, 0, 0) - ht(i, 2, 2);
            tght(j, 6) = visc11_3 * 2.0 * ht(i, 0, 1);
            tght(j, 7) = visc11_3 * 2.0 * ht(i, 0, 2);
            tght(j, 8) = visc11_3 * 2.0 * ht(i, 1, 2);
            tght(j, 9) = visc11_3 * ht(i, 1, 1) - ht(i, 2, 2);
        }

        double c13x11g = 1. / 3. * x11g;
        double c2y11g = 2.0 * y11g;
        double xm2y11g = x11g - c2y11g;
        double comd11 = ee(0, 0) * xm2y11g;
        double comd22 = ee(1, 1) * xm2y11g;
        double comd33 = ee(2, 2) * xm2y11g;
        double c2ymx11 = c2y11g - c13x11g;
        double con34 = comd11 - c13x11g;
        double con56 = comd11 + y11g;
        double con712 = comd22 + y11g;
        double con89 = comd33 + y11g;
        double con1011 = comd22 - c13x11g;

        tght(0, 0) = visc11_3 * d(0) * (comd11 + c2ymx11);
        tght(0, 1) = visc11_3 * d(1) * con56;
        tght(0, 2) = visc11_3 * d(2) * con56;
        tght(0, 3) = visc11_3 * d(0) * ee(1, 2) * xm2y11g;
        tght(0, 4) = visc11_3 * d(0) * con1011;
        tght(1, 0) = visc11_3 * d(1) * con34;
        tght(1, 1) = visc11_3 * d(0) * con712;
        tght(1, 2) = tght(0, 3);
        tght(1, 3) = visc11_3 * d(2) * con712;
        tght(1, 4) = visc11_3 * d(1) * (comd22 + c2ymx11);
        tght(1, 0) = visc11_3 * d(2) * con34;
        tght(2, 1) = tght(0, 3);
        tght(2, 2) = visc11_3 * d(0) * con89;
        tght(2, 3) = visc11_3 * d(1) * con89;
        tght(2, 4) = visc11_3 * d(2) * con1011;

        //*** insert gt21.

        double c13x12g = 1. / 3. * x12g;
        double c2y12g = 2.0 * y12g;
        double xm2y12g = x12g - c2y12g;
        double cumd11 = ee(0, 0) * xm2y12g;
        double cumd22 = ee(1, 1) * xm2y12g;
        double cumd33 = ee(2, 2) * xm2y12g;
        double c2ymx12 = c2y12g - c13x12g;
        double cun34 = cumd11 - c13x12g;
        double cun56 = cumd11 + y12g;
        double cun712 = cumd22 + y12g;
        double cun89 = cumd33 + y12g;
        double cun1011 = cumd22 - c13x12g;

        tght(6, 0) = visc12_3 * d(0) * (cumd11 + c2ymx12);
        tght(6, 1) = visc12_3 * d(1) * cun56;
        tght(6, 2) = visc12_3 * d(2) * cun56;
        tght(6, 3) = visc12_3 * d(0) * ee(1, 2) * xm2y12g;
        tght(6, 4) = visc12_3 * d(0) * cun1011;
        tght(7, 0) = visc12_3 * d(1) * cun34;
        tght(7, 1) = visc12_3 * d(0) * cun712;
        tght(7, 2) = tght(6, 3);
        tght(7, 3) = visc12_3 * d(2) * cun712;
        tght(7, 4) = visc12_3 * d(1) * (cumd22 + c2ymx12);
        tght(8, 0) = visc12_3 * d(2) * cun34;
        tght(8, 1) = tght(6, 3);
        tght(8, 2) = visc12_3 * d(0) * cun89;
        tght(8, 3) = visc12_3 * d(1) * cun89;
        tght(8, 4) = visc12_3 * d(2) * cun1011;

        //*** insert ht11.

        double d11md22 = ee(0, 0) - ee(1, 1);
        double d22md33 = ee(1, 1) - ee(2, 2);
        double d33md11 = ee(2, 2) - ee(0, 0);
        double y11hd12 = y11h * ee(0, 1);
        double y11hd13 = y11h * ee(0, 2);
        double y11hd23 = y11h * ee(1, 2);
        double cyhd12a = 2.0 * y11hd12;

        tght(3, 0) = 0.0;
        tght(3, 1) = -visc11_3 * y11hd13;
        tght(3, 2) = visc11_3 * y11hd12;
        tght(3, 3) = visc11_3 * y11h * d22md33;
        tght(3, 4) = -visc11_3 * 2.0 * y11hd23;
        tght(4, 0) = visc11_3 * 2.0 * y11hd13;
        tght(4, 1) = visc11_3 * y11hd23;
        tght(4, 2) = visc11_3 * y11h * d33md11;
        tght(4, 3) = -visc11_3 * y11hd12;
        tght(4, 4) = 0.0;
        tght(5, 0) = -visc11_3 * cyhd12a;
        tght(5, 1) = visc11_3 * y11h * d11md22;
        tght(5, 2) = -visc11_3 * y11hd23;
        tght(5, 3) = visc11_3 * y11hd13;
        tght(5, 4) = visc11_3 * cyhd12a;

        //*** insert ht12.

        double y12hd12 = y12h * ee(0, 1);
        double y12hd13 = y12h * ee(0, 2);
        double y12hd23 = y12h * ee(1, 2);
        double cyhd12b = 2.0 * y12hd12;

        tght(3, 5) = 0.0;
        tght(3, 6) = -visc12_3 * y12h * ee(0, 2);
        tght(3, 7) = visc12_3 * y12h * ee(0, 1);
        tght(3, 8) = visc12_3 * y12h * d22md33;
        tght(3, 9) = -visc12_3 * 2.0 * y12hd23;
        tght(4, 5) = visc12_3 * 2.0 * y12hd13;
        tght(4, 6) = visc12_3 * y12hd23;
        tght(4, 7) = visc12_3 * y12h * d33md11;
        tght(4, 8) = -visc12_3 * y12hd12;
        tght(4, 9) = 0.0;
        tght(5, 5) = -visc12_3 * cyhd12b;
        tght(5, 6) = visc12_3 * y12h * d11md22;
        tght(5, 7) = -visc12_3 * y12hd23;
        tght(5, 8) = visc12_3 * y12hd13;
        tght(5, 9) = visc12_3 * cyhd12b;

        //***************************************************************************c
        //***************************************************************************c
        //*** insert gt12 (=-gt21), gt22(=-gt11), ht21 (=ht12), ht22 (=ht11).

        for (std::size_t i = 0; i < 3; ++i) {
            std::size_t i3 = i + 3;
            std::size_t i6 = i + 6;
            std::size_t i9 = i + 9;

            for (std::size_t j = 0; j < 5; ++j) {
                std::size_t j5 = j + 5;

                tght(i, j5) = -tght(i6, j);
                tght(i6, j5) = -tght(i, j);
                tght(i9, j) = tght(i3, j5);
                tght(i9, j5) = tght(i3, j);
            }
        }
        //***************************************************************************c
        //***************************************************************************c
        //**************************** form tzm for rse
        //*****************************c
        multi_array<T, 3, 3, 3, 3> m;
        for (std::size_t i = 0; i < 3; ++i) {
            for (std::size_t j = i; j < 3; ++j) {
                for (std::size_t k = 0; k < 3; ++k) {
                    for (std::size_t l = k; l < 3; ++l) {
                        m(i, j, k, l) =
                            T{3. / 2.} * xm *
                                (ee(i, j) - T{1. / 3.} * delta(i, j)) *
                                (ee(k, l) - T{1. / 3.} * delta(k, l)) +
                            T{1. / 2.} * ym *
                                (ee(i, k) * delta(j, l) +
                                 ee(j, k) * delta(i, l) +
                                 ee(i, l) * delta(j, k) +
                                 ee(j, l) * delta(i, k) -
                                 T{4.0} * ee(i, j) * ee(k, l)) +
                            T{1. / 2.} * zm *
                                (delta(i, k) * delta(j, l) +
                                 delta(j, k) * delta(i, l) -
                                 delta(i, j) * delta(k, l) +
                                 ee(i, j) * delta(k, l) +
                                 ee(k, l) * delta(i, j) -
                                 ee(i, k) * delta(j, l) -
                                 ee(j, k) * delta(i, l) -
                                 ee(i, l) * delta(j, k) -
                                 ee(j, l) * delta(i, k) + ee(i, j) * ee(k, l));
                    }
                }
            }
        }

        for (std::size_t i = 0; i < 5; ++i) {

            if (i == 0 || i == 4) {
                tzm(i, 0) = m(mesid(0, i), mesid(0, i), 0, 0) -
                            m(mesid(0, i), mesid(0, i), 2, 2) -
                            (m(mesid(1, i), mesid(1, i), 0, 0) -
                             m(mesid(1, i), mesid(1, i), 2, 2));
                tzm(i, 1) = 2.0 * (m(mesid(0, i), mesid(0, i), 0, 1) -
                                   m(mesid(1, i), mesid(1, i), 0, 1));
                tzm(i, 2) = 2.0 * (m(mesid(0, i), mesid(0, i), 0, 2) -
                                   m(mesid(1, i), mesid(1, i), 0, 2));
                tzm(i, 3) = 2.0 * (m(mesid(0, i), mesid(0, i), 1, 2) -
                                   m(mesid(1, i), mesid(1, i), 1, 2));
                tzm(i, 4) = m(mesid(0, i), mesid(0, i), 1, 1) -
                            m(mesid(0, i), mesid(0, i), 2, 2) -
                            (m(mesid(1, i), mesid(1, i), 1, 1) -
                             m(mesid(1, i), mesid(1, i), 2, 2));

            } else {

                tzm(i, 0) = 2.0 * (m(mesid(0, i), mesid(1, i), 0, 0) -
                                   m(mesid(0, i), mesid(1, i), 2, 2));
                tzm(i, 1) = 4.0 * m(mesid(0, i), mesid(1, i), 0, 1);
                tzm(i, 2) = 4.0 * m(mesid(0, i), mesid(1, i), 0, 2);
                tzm(i, 3) = 4.0 * m(mesid(0, i), mesid(1, i), 1, 2);
                tzm(i, 4) = 2.0 * (m(mesid(0, i), mesid(1, i), 1, 1) -
                                   m(mesid(0, i), mesid(1, i), 2, 2));
            }
        }
        //***************************************************************************c
        //***************************************************************************c
        //*** fill in upper half of m12 (=m11) and m22 (=m11).

        for (std::size_t j = 0; j < 5; ++j) {

            std::size_t j5 = j + 5;

            for (std::size_t i = 0; i < j + 1; ++i) {

                std::size_t i5 = i + 5;

                tzm(i, j5) = visc12_3 * tzm(i, j);
                tzm(i5, j5) = visc22_3 * tzm(i, j);
            }
        }
        //*** fill in the lower half of m12.
        for (std::size_t i = 0; i < 5; ++i) {

            std::size_t i5 = i + 5;
            for (std::size_t j = i + 1; j < 5; ++j) {

                std::size_t j5 = j + 5;

                tzm(j, i5) = visc12_3 * tzm(i, j5);
            }
        }

        for (std::size_t i = 0; i < 5; ++i) {
            for (std::size_t j = 0; j < 5; ++j) {
                tzm(i, j) = visc11_3 * tzm(i, j);
            }
        }
    }
};

template <typename T>
struct thermalizer {
    T sqrt_kT_Dt;
    std::size_t offset;
    std::size_t seed;
#ifdef __CUDACC__
    __device__
#endif
    T operator()(std::size_t index) {
#ifdef __CUDACC__
        uint4 rnd_ints = curand_Philox4x32_10(make_uint4(offset >> 32, seed >> 32, index >> 32, index),
                                              make_uint2(offset, seed));
        T rnd = _curand_uniform_double_hq(rnd_ints.w, rnd_ints.x);
#else
        // get two 64-bit random unsigned integers (of which only one is used)
        RNG rng;
        RNG::ctr_type c;
        RNG::key_type k;
        k[0] = seed;
        c[0] = offset;
        c[1] = index;
        RNG::ctr_type rint = rng(c, k);
        
        // convert to uniform distribution
        uint64_t constexpr const max = std::numeric_limits<uint64_t>::max();
        double constexpr const fac = 1. / (max + 1.);
        T rnd = (rint[0] + 0.5) * fac;
#endif
        return std::sqrt(T{2.0}) * sqrt_kT_Dt * std::sqrt(T{12.0}) * (rnd - 0.5);

        // sqrt(12) * (rnd - 0.5) is a uniformly distributed random number
        // with zero mean and unit variance.
        
        // sqrt(2 * kT * \Delta t) is the desired standard deviation for 
        // random displacement. 

        // NOTE: Here, we do not compute a random displacement but a random 
        // velocity, therefore the standard deviation has been divided by 
        // the time step (see sd_interface.cpp).
        
        // why use the _curand_uniform_double_hq function?
        // I don't think it is meant to be used outside of the
        // curand_kernel.h file.
        // Apparently it works, so keep it for now.
    }
};

/** Functor that takes all the relevant particle data (i.e. positions, radii,
    external forces and torques) and computes the translational and angular
    velocities. These can be used to propagate the system.

    The basic idea of the method is as follows: The Stokes equation is linear,
    i.e. the relations between force and motion (velocity) is purely linear.
    All forces and torques that act on individual particles are merged into
    one large vector. All velocities and angular velocities of individual
    particles are merged into one vector alike.
    The relationship between force and velocity is now given by the so-called
    grand mobility matrix, or its inverse, the resistance matrix:

            \f[\vec{U} = M_{UF} \vec{F}\f]

    where \f$\vec{U}\f$ is the force, \f$\vec{F}\f$ the velocities and \f$M\f$
    the grand mobility matrix. The subscript \f$_{UF}\f$ indicates that this
    tensor describes the relationship between forces and velocities.
    the relationship between forces and velocities.
    The mobility matrix depends on the positions of all particles and therefore
    is only valid for the momentary configuration. It needs to be recalculated
    each time step.

    To obtain the velocities, the mobility matrix must be applied to the forces
    that act on the particles, giving the velocities.

    These are the steps that are taken to compute the mobility matrix:
      -# Starting point is an empty mobility matrix.
      -# All self mobility terms are added. Its translational part is widely
         known as Stoke's Law.
      -# All pair mobility terms are added.
      -# The mobility matrix is inverted to become a resistance matrix.
      -# Lubrication corrections are added to the resistance matrix. They are
         necessary for particles in close vicinity because simplifications
         have been made to efficiently cover long range interactions.
      -# In the end, the finished resistance matrix is inverted again. The
         result is the finished mobility matrix that includes the short-ranged
         lubrication interactions.

    The thermalisation is achieved by a generalization of the 
    Einstein-Smoluchowski equation, which relates mobility and diffusion: 

            \f[D = \mu k_B T\f]

    where \f$\mu\f$ is the mobility and \f$D\f$ is the diffusion coefficient.
    The mean square displacement during a time step of length \f$\Delta t\f$,
    and along one degree of freedom is given by

            \f[\langle x^2 \rangle / \Delta t = 2 D\f]

    That way we can determine the distribution of the random displacement
    that our system experiences along one of its many degrees of freedom. In
    our case, \f$\mu\f$ is a matrix and we need its square root. The latter is
    obtained via Cholesky decomposition.
 */
template <typename Policy, typename T>
struct solver {
    static_assert(policy::is_policy<Policy>::value,
                  "The execution policy must meet the requirements");
    static_assert(std::is_arithmetic<T>::value,
                  "Data type of device_matrix must be arithmetic for "
                  "arithmetic operations");

    template <typename U>
    using vector_type = typename Policy::template vector<U>;
    
    /** viscosity of the Stokes fluid */
    T const eta;
    /** number of particles */
    std::size_t const n_part;
    /** number of pairs of particles = n_part*(n_part-1)/2 */
    std::size_t const n_pair;

    device_matrix<T, Policy> zmuf;
    device_matrix<T, Policy> zmus;
    device_matrix<T, Policy> zmes;

    solver(T eta, std::size_t const n_part)
        : eta{eta}, n_part(n_part), n_pair(n_part * (n_part - 1) / 2),
          zmuf(n_part * 6, n_part * 6), zmus(n_part * 6, n_part * 5),
          zmes(n_part * 5, n_part * 5) {}

    std::vector<T> calc_vel(std::vector<T> const &x_host,
                            std::vector<T> const &f_host,
                            std::vector<T> const &a_host,
                            T sqrt_kT_Dt,
                            std::size_t offset,
                            std::size_t seed,
                            int const flg = SELF_MOBILITY | PAIR_MOBILITY | FTS) {
        assert(x_host.size() == 6 * n_part);
        vector_type<T> x(x_host.begin(), x_host.end());
        assert(a_host.size() == n_part);
        vector_type<T> a(a_host.begin(), a_host.end());

        // TODO: More efficient!
        // generate lookup table of all pairs
        device_matrix<std::size_t, Policy> part_id(2, n_pair);
        std::size_t k = 0;
        for (std::size_t i = 0; i < n_part; ++i) {
            for (std::size_t j = i + 1; j < n_part; ++j) {
                part_id(0, k) = i;
                part_id(1, k) = j;
                k += 1;
            }
        }

        device_matrix<T, Policy> pd(4, n_pair);
        thrust_wrapper::counting_iterator<std::size_t> begin(0UL);

        // check_dist
        thrust_wrapper::for_each(Policy::par(), begin, begin + n_pair,
                         check_dist<Policy, T>{x, a, pd, part_id});

        // self mobility term
        if (flg & SELF_MOBILITY) {
            thrust_wrapper::for_each(
                Policy::par(), begin, begin + n_part,
                mobility<Policy, T, true>{zmuf, zmus, zmes, a, eta, flg});
        }

        // pair mobility term
        if (flg & PAIR_MOBILITY) {
            thrust_wrapper::for_each(Policy::par(), begin, begin + n_pair,
                                     mobility<Policy, T, false>{zmuf, zmus, zmes, pd,
                                                                part_id, a, eta, flg});
        }


        
//        printf("zmuf:\n");
//        for (int i=0; i<zmuf.rows(); i++) {
//            for (int j=0; j<zmuf.cols(); j++) {
//                printf("%f ",zmuf(i,j));
//            }
//            printf("\n");
//        }
//        printf("\n");

        // Invert the grand-mobility tensor.  This is done in several steps
        // which minimize the computation time.

        // Invert R1 = Muf ^ -1 => zmuf = zmuf ^ -1
        zmuf = zmuf.inverse();

//        printf("zmuf:\n");
//        for (int i=0; i<zmuf.rows(); i++) {
//            for (int j=0; j<zmuf.cols(); j++) {
//                printf("%f ",zmuf(i,j));
//            }
//            printf("\n");
//        }
//        printf("\n");

        if (flg & FTS) {
            // Compute R2 = Mus(t) * R1 => rsu = zmus(t) * zmuf
            device_matrix<T, Policy> const &rsu = zmus.transpose() * zmuf;

            // Compute R3 = R2 * Mus - Mes => zmes = rsu * zmus - zmes
            zmes = zmes - rsu * zmus;

            // Invert  R4 = R3 ^ -1 => zmes = zmes ^ -1
            zmes = zmes.inverse();

            // Compute R5 = -R3 * R4 => zmus = -rsu(t) * zmes
            zmus = -(rsu.transpose() * zmes);

            // Compute R6 = R1 - R5  => zmuf = zmuf - zmus * rsu
            zmuf = zmuf - zmus * rsu;
        }

        device_matrix<T, Policy> rfu = zmuf;
        device_matrix<T, Policy> rfe = zmus;
        device_matrix<T, Policy> rse = zmes;

//        printf("rfu:\n");
//        for (int i=0; i<rfu.rows(); i++) {
//            for (int j=0; j<rfu.cols(); j++) {
//                printf("%f ",rfu(i,j));
//            }
//            printf("\n");
//        }
//        printf("\n");

        // Lubrication corrections (equation (2.18) or (2.21) resp.)
        if (flg & LUBRICATION) {
            thrust_wrapper::for_each(Policy::par(), begin, begin + n_pair,
                                     lubrication<Policy, T>{rfu, rfe, rse, pd, part_id,
                                                            a, eta, flg});

            for (std::size_t i = 0; i < 6 * n_part; ++i) {
                for (std::size_t j = 0; j < i; ++j) {
                    rfu(i, j) = rfu(j, i);
                }
            }

            for (std::size_t i = 0; i < 5 * n_part; ++i) {
                for (std::size_t j = 0; j < i; ++j) {
                    rse(i, j) = rse(j, i);
                }
            }
        }

//        printf("rfu:\n");
//        for (int i=0; i<rfu.rows(); i++) {
//            for (int j=0; j<rfu.cols(); j++) {
//                printf("%f ",rfu(i,j));
//            }
//            printf("\n");
//        }
//        printf("\n");


        assert(f_host.size() == 6 * n_part);
        vector_type<T> fext(f_host.begin(), f_host.end());
        
        // initialize ambient flow
        vector_type<T> uinf(n_part * 6, T{0.0});
        // initialize ambient shear flow
        vector_type<T> einf(n_part * 5, T{0.0});
        // Note: if we were to implement the case einf != 0 we would need to
        // initialize the ambient flow according to the particle's positions.
        // E.g. like   uinf_i = einf * r_i   where i is particle index.

        device_matrix<T, Policy> rfu_inv;
        device_matrix<T, Policy> rfu_sqrt;
        thrust_wrapper::tie(rfu_inv, rfu_sqrt) = rfu.inverse_and_cholesky();

        vector_type<T> frnd(n_part * 6, T{0.0});

        // Thermalization with stochastic force
        if (sqrt_kT_Dt > 0.0) {

            // This method is combined from two locations,
            // namely @cite banchio03a, 
            // Banchio, Brady 2002 https://doi.org/10.1063/1.1571819
            // equation (6)
            //  -- and --
            // @cite brady88a
            // Brady, Bossis 1988
            // https://doi.org/10.1146/annurev.fl.20.010188.000551
            // equation (3)

            // We adopt the more detailed method of the paper @cite banchio03a.

            // However, the matrix A in that paper is NOT a byproduct of the
            // matrix inversion of R_{FU} as they claim. (We get the
            // decomposition of R_{FU} but not of its inverse)
            // Also, the random displacement between formulas (5) and (6) is
            // wrong, since the square root needs to include kT and Delta t
            // as well.

            // Luckily, with the decomposition of R_{FU} we CAN compute an
            // appropriate random force with the variance given by the paper
            // @cite brady88a (as opposed to random displacement).

            vector_type<T> psi(f_host.size());
            thrust_wrapper::tabulate(Policy::par(), psi.begin(), psi.end(),
                             thermalizer<T>{sqrt_kT_Dt, offset, seed});

            frnd = rfu_sqrt * psi;

            // There is possibly an additional term for the thermalization
            //
            //     \nabla \cdot R_{FU}^{-1} \Delta t
            //
            // But this seems to be omitted in most cases in the literature.
            // It is also very unclear how to actually calculate it.
        }

        // This is equation (2.22), plus thermal forces.

        vector_type<T> u = rfu_inv * (fext + rfe * einf + frnd) + uinf;

        // return the velocities due to hydrodynamic interactions
        std::vector<T> out(u.size());
        thrust_wrapper::copy(u.begin(), u.end(), out.begin());

        return out;
    }
};

} // namespace sd
