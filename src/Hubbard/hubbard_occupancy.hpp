// Copyright (c) 2013-2018 Mathieu Taillefumier, Anton Kozhevnikov, Thomas Schulthess
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are permitted provided that
// the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the
//    following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions
//    and the following disclaimer in the documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
// WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
// PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
// OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

/** \file hubbard_occupancy.hpp
 *
 *  \brief Generate occupation matrix for Hubbard orbitals.
 */

/** Compute the occupation numbers associated to the hubbard wavefunctions (locally centered orbitals, wannier
 *  functions, etc) that are relevant for the hubbard correction.
 *
 * These quantities are defined by
 * \f[
 *    n_{m,m'}^I \sigma = \sum_{kv} f(\varepsilon_{kv}) |<\psi_{kv}| phi_I_m>|^2 
 * \f]
 * where \f[m=-l\cdot l$ (same for m')\f], I is the atom.
 *
 * Requires symmetrization. */
void Hubbard::hubbard_compute_occupation_numbers(K_point_set& kset_)
{
    if (!ctx_.hubbard_correction()) {
        return;
    }

    this->occupancy_number_.zero();

    int HowManyBands = -1;
    /* if we are doing calculations for non colinear magnetism or
       simple LDA then do not change the number of bands. the factor
       two is important for colinear magnetism since the up-up and
       down-down blocks are decoupled but the wave-functions are up
       and down are still stored as a spinor to conserve space. */
    for (int ikloc = 0; ikloc < kset_.spl_num_kpoints().local_size(); ikloc++) {
        int  ik = kset_.spl_num_kpoints(ikloc);
        auto kp = kset_[ik];

        HowManyBands = std::max(kp->num_occupied_bands(0), HowManyBands);
        if (ctx_.num_mag_dims() == 1) {
            HowManyBands = std::max(kp->num_occupied_bands(1), HowManyBands);
        }
    }

    /* now for each spin components and each atom we need to calculate
     <psi_{nk}|phi^I_m'><phi^I_m|psi_{nk}> */
    int Ncf = 1;
    if (ctx_.num_mag_dims() == 1) {
        /* !!! colinear magnetism. We need to have 2 times more space. */
        Ncf = 2;
    }

    dmatrix<double_complex> dm(HowManyBands, this->number_of_hubbard_orbitals() * Ncf);
    matrix<double_complex>  dm1(HowManyBands, this->number_of_hubbard_orbitals() * Ncf);
    matrix<double_complex>  Op(this->number_of_hubbard_orbitals() * Ncf, this->number_of_hubbard_orbitals() * Ncf);

    dm.zero();

    #ifdef __GPU
    if (ctx_.processing_unit() == GPU) {
        /* the communicator is always of size 1.  I need to allocate memory
           on the device manually */
        dm.allocate(memory_t::device);
    }
    #endif

    for (int ikloc = 0; ikloc < kset_.spl_num_kpoints().local_size(); ikloc++) {
        int  ik = kset_.spl_num_kpoints(ikloc);
        auto kp = kset_[ik];
        #ifdef __GPU
        if (ctx_.processing_unit() == GPU) {
            for (int ispn = 0; ispn < ctx_.num_spins(); ispn++) {
                /* allocate GPU memory */
                kp->spinor_wave_functions().pw_coeffs(ispn).prime().allocate(memory_t::device);
                kp->spinor_wave_functions().pw_coeffs(ispn).copy_to_device(0, kp->num_occupied_bands(ispn));
            }

            for (int ispn = 0; ispn < kp->hubbard_wave_functions().num_sc(); ispn++) {

                if (!kp->hubbard_wave_functions().pw_coeffs(ispn).prime().on_device()) {
                    kp->hubbard_wave_functions().pw_coeffs(ispn).prime().allocate(memory_t::device);
                }

                kp->hubbard_wave_functions().pw_coeffs(ispn).copy_to_device(0, this->number_of_hubbard_orbitals());
            }
        }
        #endif
        dm.zero();
        if (ctx_.num_mag_dims() == 3) {
            inner(ctx_.processing_unit(), 2, kp->spinor_wave_functions(), 0, kp->num_occupied_bands(), kp->hubbard_wave_functions(), 0,
                  this->number_of_hubbard_orbitals(), dm, 0, 0);
        } else {
            // SLDA + U, we need to do the explicit calculation. The
            // hubbard orbitals only have one component while the bloch
            // wave functions have two. The inner product takes care of
            // this case internally.
            for (int ispn_ = 0; ispn_ < ctx_.num_spins(); ispn_++) {
                inner(ctx_.processing_unit(), ispn_, kp->spinor_wave_functions(), 0, kp->num_occupied_bands(ispn_),
                      kp->hubbard_wave_functions(), 0, this->number_of_hubbard_orbitals(), dm, 0,
                      ispn_ * this->number_of_hubbard_orbitals());
            }
        }

        #ifdef __GPU
        if (ctx_.processing_unit() == GPU) {
            for (int ispn = 0; ispn < ctx_.num_spins(); ispn++) {
                /* deallocate GPU memory */
                kp->spinor_wave_functions().pw_coeffs(ispn).deallocate_on_device();
            }

            for (int ispn = 0; ispn < kp->hubbard_wave_functions().num_sc(); ispn++) {
                kp->hubbard_wave_functions().pw_coeffs(ispn).deallocate_on_device();
            }

            dm.copy<memory_t::device, memory_t::host>();
        }
        #endif

        // compute O'_{nk,j} = O_{nk,j} * f_{nk}
        // NO summation over band yet

        dm1.zero(); // O'

        if (ctx_.num_mag_dims() == 3) {
            #pragma omp parallel for
            for (int m = 0; m < this->number_of_hubbard_orbitals(); m++) {
                for (int nband = 0; nband < kp->num_occupied_bands(0); nband++) {
                    dm1(nband, m) = dm(nband, m) * kp->band_occupancy(nband, 0);
                }
            }
        } else {
            #pragma omp parallel for
            for (int m = 0; m < this->number_of_hubbard_orbitals(); m++) {
                for (int ispn = 0; ispn < ctx_.num_spins(); ispn++) {
                    for (int nband = 0; nband < kp->num_occupied_bands(ispn); nband++) {
                        dm1(nband, ispn * this->number_of_hubbard_orbitals() + m) =
                            dm(nband, ispn * this->number_of_hubbard_orbitals() + m) * kp->band_occupancy(nband, ispn);
                    }
                }
            }
        }

        // now compute O_{ij}^{sigma,sigma'} = \sum_{nk} <psi_nk|phi_{i,sigma}><phi_{j,sigma^'}|psi_nk> f_{nk}
        const double scal = (ctx_.num_mag_dims() == 0) ? 0.5 : 1.0;
        linalg<CPU>::gemm(2, 0, this->number_of_hubbard_orbitals() * Ncf, this->number_of_hubbard_orbitals() * Ncf, HowManyBands,
                          double_complex(kp->weight() * scal, 0.0), dynamic_cast<matrix<double_complex>&>(dm), dm1,
                          linalg_const<double_complex>::zero(), Op);

        if (ctx_.num_mag_dims() == 3) {
            // there must be a way to do that with matrix multiplication
            #pragma omp parallel for schedule(static)
            for (int ia = 0; ia < unit_cell_.num_atoms(); ia++) {
                const auto& atom = unit_cell_.atom(ia);
                if (atom.type().hubbard_correction()) {
                    const int lmax_at = 2 * atom.type().hubbard_orbital(0).hubbard_l() + 1;
                    for (int s1 = 0; s1 < ctx_.num_spins(); s1++) {
                        for (int s2 = 0; s2 < ctx_.num_spins(); s2++) {
                            int s = (s1 == s2) * s1 + (s1 != s2) * (1 + 2 * s2 + s1);
                            for (int mp = 0; mp < lmax_at; mp++) {
                                for (int m = 0; m < lmax_at; m++) {
                                    this->occupancy_number_(m, mp, s, ia, 0) +=
                                        Op(this->offset[ia] + m + s1 * lmax_at, this->offset[ia] + mp + s2 * lmax_at);
                                }
                            }
                        }
                    }
                }
            }
        } else {
            // Well we need to apply a factor 1/2 (the constant scal
            // above) when we compute the occupancies for the boring LDA
            // + U. It is because the calculations of E and U consider
            // occupancies <= 1.  Sirius for the boring lda+U has a
            // factor 2 in the kp band occupancies. We need to
            // compensate for it because it is taken into account in the
            // calculation of the hubbard potential

            #pragma omp parallel for schedule(static)
            for (int ia = 0; ia < unit_cell_.num_atoms(); ia++) {
                const auto& atom = unit_cell_.atom(ia);
                if (atom.type().hubbard_correction()) {
                    const int lmax_at = 2 * atom.type().hubbard_orbital(0).hubbard_l() + 1;
                    for (int ispn = 0; ispn < ctx_.num_spins(); ispn++) {
                        for (int mp = 0; mp < lmax_at; mp++) {
                            const int mmp = this->offset[ia] + mp + ispn * this->number_of_hubbard_orbitals();
                            for (int m = 0; m < lmax_at; m++) {
                                const int mm = this->offset[ia] + m + ispn * this->number_of_hubbard_orbitals();
                                this->occupancy_number_(m, mp, ispn, ia, 0) += Op(mm, mmp);
                            }
                        }
                    }
                }
            }
        }
    }

    #ifdef __GPU
    if (ctx_.processing_unit() == GPU) {
        dm.deallocate(memory_t::device);
    }
    #endif

    // global reduction over k points
    ctx_.comm_k().allreduce<double_complex, mpi_op_t::sum>(this->occupancy_number_.at<CPU>(),
                                                           static_cast<int>(this->occupancy_number_.size()));

    // Now symmetrization procedure. We need to review that

    if (0) {
        if (ctx_.num_mag_dims() == 3) {
            symmetrize_occupancy_matrix_noncolinear_case();
        } else {
            symmetrize_occupancy_matrix();
        }
    }

    print_occupancies();
}

// The initial occupancy is calculated following Hund rules. We first
// fill the d (f) states according to the hund's rules and with majority
// spin first and the remaining electrons distributed among the minority
// states.
void Hubbard::calculate_initial_occupation_numbers()
{
    this->occupancy_number_.zero();
    #pragma omp parallel for schedule(static)
    for (int ia = 0; ia < unit_cell_.num_atoms(); ia++) {
        const auto& atom = unit_cell_.atom(ia);
        if (atom.type().hubbard_correction()) {
            const int lmax_at = 2 * atom.type().hubbard_orbital(0).hubbard_l() + 1;
            // compute the total charge for the hubbard orbitals
            double charge = atom.type().hubbard_orbital(0).hubbard_occupancy();
            bool   nm     = true; // true if the atom is non magnetic
            int    majs, mins;
            if (ctx_.num_spins() != 1) {
                if (atom.vector_field()[2] > 0.0) {
                    nm   = false;
                    majs = 0;
                    mins = 1;
                } else if (atom.vector_field()[2] < 0.0) {
                    nm   = false;
                    majs = 1;
                    mins = 0;
                }
            }

            if (!nm) {
                if (ctx_.num_mag_dims() != 3) {
                    // colinear case

                    if (charge > (lmax_at)) {
                        for (int m = 0; m < lmax_at; m++) {
                            this->occupancy_number_(m, m, majs, ia, 0) = 1.0;
                            this->occupancy_number_(m, m, mins, ia, 0) =
                                (charge - static_cast<double>(lmax_at)) / static_cast<double>(lmax_at);
                        }
                    } else {
                        for (int m = 0; m < lmax_at; m++) {
                            this->occupancy_number_(m, m, majs, ia, 0) = charge / static_cast<double>(lmax_at);
                        }
                    }
                } else {
                    // double c1, s1;
                    // sincos(atom.type().starting_magnetization_theta(), &s1, &c1);
                    double         c1 = atom.vector_field()[2];
                    double_complex cs = double_complex(atom.vector_field()[0], atom.vector_field()[1]) / sqrt(1.0 - c1 * c1);
                    double_complex ns[4];

                    if (charge > (lmax_at)) {
                        ns[majs] = 1.0;
                        ns[mins] = (charge - static_cast<double>(lmax_at)) / static_cast<double>(lmax_at);
                    } else {
                        ns[majs] = charge / static_cast<double>(lmax_at);
                        ns[mins] = 0.0;
                    }

                    // charge and moment
                    double nc  = ns[majs].real() + ns[mins].real();
                    double mag = ns[majs].real() - ns[mins].real();

                    // rotate the occ matrix
                    ns[0] = (nc + mag * c1) * 0.5;
                    ns[1] = (nc - mag * c1) * 0.5;
                    ns[2] = mag * std::conj(cs) * 0.5;
                    ns[3] = mag * cs * 0.5;

                    for (int m = 0; m < lmax_at; m++) {
                        this->occupancy_number_(m, m, 0, ia, 0) = ns[0];
                        this->occupancy_number_(m, m, 1, ia, 0) = ns[1];
                        this->occupancy_number_(m, m, 2, ia, 0) = ns[2];
                        this->occupancy_number_(m, m, 3, ia, 0) = ns[3];
                    }
                }
            } else {
                for (int s = 0; s < ctx_.num_spins(); s++) {
                    for (int m = 0; m < lmax_at; m++) {
                        this->occupancy_number_(m, m, s, ia, 0) = charge * 0.5 / static_cast<double>(lmax_at);
                    }
                }
            }
        }
    }

    print_occupancies();
}

inline void Hubbard::print_occupancies()
{
    if (ctx_.control().verbosity_ > 1 && ctx_.comm().rank() == 0) {
        printf("\n");
        for (int ci = 0; ci < 10; ci++) {
            printf("--------");
        }
        printf("\n");
        printf("hubbard occupancies\n");
        for (int ia = 0; ia < unit_cell_.num_atoms(); ia++) {
            printf("Atom : %d\n", ia);
            printf("Mag Dim : %d\n", ctx_.num_mag_dims());
            const auto& atom = unit_cell_.atom(ia);

            if (atom.type().hubbard_correction()) {
                const int lmax_at = 2 * atom.type().hubbard_orbital(0).hubbard_l() + 1;
                for (int m1 = 0; m1 < lmax_at; m1++) {
                    for (int m2 = 0; m2 < lmax_at; m2++) {
                        printf("%.3lf ", std::abs(this->occupancy_number_(m1, m2, 0, ia, 0)));
                    }

                    if (ctx_.num_mag_dims() == 3) {
                        printf(" ");
                        for (int m2 = 0; m2 < lmax_at; m2++) {
                            printf("%.3lf ", std::abs(this->occupancy_number_(m1, m2, 2, ia, 0)));
                        }
                    }
                    printf("\n");
                }

                if (ctx_.num_spins() == 2) {
                    for (int m1 = 0; m1 < lmax_at; m1++) {
                        if (ctx_.num_mag_dims() == 3) {
                            for (int m2 = 0; m2 < lmax_at; m2++) {
                                printf("%.3lf ", std::abs(this->occupancy_number_(m1, m2, 3, ia, 0)));
                            }
                            printf(" ");
                        }
                        for (int m2 = 0; m2 < lmax_at; m2++) {
                            printf("%.3lf ", std::abs(this->occupancy_number_(m1, m2, 1, ia, 0)));
                        }
                        printf("\n");
                    }
                }

                double n_up, n_down, n_total;
                n_up   = 0.0;
                n_down = 0.0;
                for (int m1 = 0; m1 < lmax_at; m1++) {
                    n_up += this->occupancy_number_(m1, m1, 0, ia, 0).real();
                }

                if (ctx_.num_spins() == 2) {
                    for (int m1 = 0; m1 < lmax_at; m1++) {
                        n_down += this->occupancy_number_(m1, m1, 1, ia, 0).real();
                    }
                }
                printf("\n");
                n_total = n_up + n_down;
                if (ctx_.num_spins() == 2) {
                    printf("Atom charge (total) %.5lf (n_up) %.5lf (n_down) %.5lf (mz) %.5lf\n", n_total, n_up, n_down, n_up - n_down);
                } else {
                    printf("Atom charge (total) %.5lf\n", 2.0 * n_total);
                }

                printf("\n");
                for (int ci = 0; ci < 10; ci++) {
                    printf("--------");
                }
                printf("\n");
            }
        }
    }
}

inline void Hubbard::symmetrize_occupancy_matrix_noncolinear_case()
{
    auto& sym = unit_cell_.symmetry();

    // check if we have some symmetries
    if (sym.num_mag_sym()) {
        int lmax  = unit_cell_.lmax();
        int lmmax = utils::lmmax(lmax);

        mdarray<double_complex, 2> rotm(lmmax, lmmax);
        mdarray<double_complex, 4> rotated_oc(lmmax, lmmax, ctx_.num_spins() * ctx_.num_spins(), unit_cell_.num_atoms());

        double alpha = 1.0 / static_cast<double>(sym.num_mag_sym());
        rotated_oc.zero();

        for (int i = 0; i < sym.num_mag_sym(); i++) {
            int  pr   = sym.magnetic_group_symmetry(i).spg_op.proper;
            auto eang = sym.magnetic_group_symmetry(i).spg_op.euler_angles;
            // int isym  = sym.magnetic_group_symmetry(i).isym;
            SHT::rotation_matrix(lmax, eang, pr, rotm);
            auto spin_rot_su2 = SHT::rotation_matrix_su2(sym.magnetic_group_symmetry(i).spin_rotation);

            #pragma omp parallel for schedule(static)
            for (int ia = 0; ia < unit_cell_.num_atoms(); ia++) {
                const auto& atom = unit_cell_.atom(ia);
                if (atom.type().hubbard_correction()) {
                    const int lmax_at = 2 * atom.type().hubbard_orbital(0).hubbard_l() + 1;
                    for (int ii = 0; ii < lmax_at; ii++) {
                        int l1 = utils::lm(atom.type().hubbard_orbital(0).hubbard_l(), ii - atom.type().hubbard_orbital(0).hubbard_l());
                        for (int ll = 0; ll < lmax_at; ll++) {
                            int                        l2 = utils::lm(atom.type().hubbard_orbital(0).hubbard_l(), ll - atom.type().hubbard_orbital(0).hubbard_l());
                            mdarray<double_complex, 1> rot_spa(ctx_.num_spins() * ctx_.num_spins());
                            rot_spa.zero();
                            for (int s1 = 0; s1 < ctx_.num_spins(); s1++) {
                                for (int s2 = 0; s2 < ctx_.num_spins(); s2++) {
                                    // symmetrization procedure
                                    // A_ij B_jk C_kl

                                    for (int jj = 0; jj < lmax_at; jj++) {
                                        int l3 = utils::lm(atom.type().hubbard_orbital(0).hubbard_l(), jj - atom.type().hubbard_orbital(0).hubbard_l());
                                        for (int kk = 0; kk < lmax_at; kk++) {
                                            int l4 = utils::lm(atom.type().hubbard_orbital(0).hubbard_l(), kk - atom.type().hubbard_orbital(0).hubbard_l());
                                            rot_spa(2 * s1 + s2) +=
                                                std::conj(rotm(l1, l3)) *
                                                occupancy_number_(jj, kk, (s1 == s2) * s1 + (s1 != s2) * (1 + 2 * s1 + s2), ia, 0) *
                                                rotm(l2, l4) * alpha;
                                        }
                                    }
                                }
                            }

                            double_complex spin_dm[2][2] = {{0.0, 0.0}, {0.0, 0.0}};
                            for (int iii = 0; iii < ctx_.num_spins(); iii++) {
                                for (int lll = 0; lll < ctx_.num_spins(); lll++) {
                                    // A_ij B_jk C_kl
                                    for (int jj = 0; jj < ctx_.num_spins(); jj++) {
                                        for (int kk = 0; kk < ctx_.num_spins(); kk++) {
                                            spin_dm[iii][lll] +=
                                                spin_rot_su2(iii, jj) * rot_spa(jj + 2 * kk) * std::conj(spin_rot_su2(kk, lll));
                                        }
                                    }
                                }
                            }

                            rotated_oc(ii, ll, 0, ia) += spin_dm[0][0];
                            rotated_oc(ii, ll, 1, ia) += spin_dm[1][1];
                            rotated_oc(ii, ll, 2, ia) += spin_dm[0][1];
                            rotated_oc(ii, ll, 3, ia) += spin_dm[1][0];
                        }
                    }
                }
            }
        }

        for (int ia = 0; ia < unit_cell_.num_atoms(); ia++) {
            auto& atom = unit_cell_.atom(ia);
            if (atom.type().hubbard_correction()) {
                const int lmax_at = 2 * atom.type().hubbard_orbital(0).hubbard_l() + 1;
                for (int ii = 0; ii < lmax_at; ii++) {
                    for (int ll = 0; ll < lmax_at; ll++) {
                        for (int s = 0; s < ctx_.num_spins() * ctx_.num_spins(); s++) {
                            this->occupancy_number_(ii, ll, s, ia, 0) = rotated_oc(ii, ll, s, ia);
                        }
                    }
                }
            }
        }
    }
}

inline void Hubbard::symmetrize_occupancy_matrix()
{
    auto& sym = unit_cell_.symmetry();

    // check if we have some symmetries
    if (sym.num_mag_sym()) {
        int lmax  = unit_cell_.lmax();
        int lmmax = utils::lmmax(lmax);

        mdarray<double_complex, 2> rotm(lmmax, lmmax);
        mdarray<double_complex, 4> rotated_oc(lmmax, lmmax, ctx_.num_spins() * ctx_.num_spins(), unit_cell_.num_atoms());

        double alpha = 1.0 / static_cast<double>(sym.num_mag_sym());
        rotated_oc.zero();

        for (int i = 0; i < sym.num_mag_sym(); i++) {
            int  pr   = sym.magnetic_group_symmetry(i).spg_op.proper;
            auto eang = sym.magnetic_group_symmetry(i).spg_op.euler_angles;
            // int isym  = sym.magnetic_group_symmetry(i).isym;
            SHT::rotation_matrix(lmax, eang, pr, rotm);
            auto spin_rot_su2 = SHT::rotation_matrix_su2(sym.magnetic_group_symmetry(i).spin_rotation);

            #pragma omp parallel for schedule(static)
            for (int ia = 0; ia < unit_cell_.num_atoms(); ia++) {
                const auto& atom = unit_cell_.atom(ia);
                if (atom.type().hubbard_correction()) {
                    const int               lmax_at = 2 * atom.type().hubbard_orbital(0).hubbard_l() + 1;
                    dmatrix<double_complex> rot_spa(lmax_at, lmax_at);
                    rot_spa.zero();
                    for (int ispn = 0; ispn < ctx_.num_spins(); ispn++) {
                        for (int ii = 0; ii < lmax_at; ii++) {
                            int l1 = utils::lm(atom.type().hubbard_orbital(0).hubbard_l(), ii - atom.type().hubbard_orbital(0).hubbard_l());
                            for (int ll = 0; ll < lmax_at; ll++) {
                                int l2 = utils::lm(atom.type().hubbard_orbital(0).hubbard_l(), ll - atom.type().hubbard_orbital(0).hubbard_l());
                                // symmetrization procedure
                                // A_ij B_jk C_kl
                                for (int kk = 0; kk < lmax_at; kk++) {
                                    int l4 = utils::lm(atom.type().hubbard_orbital(0).hubbard_l(), kk - atom.type().hubbard_orbital(0).hubbard_l());
                                    for (int jj = 0; jj < lmax_at; jj++) {
                                        int l3 = utils::lm(atom.type().hubbard_orbital(0).hubbard_l(), jj - atom.type().hubbard_orbital(0).hubbard_l());
                                        rot_spa(ii, kk) +=
                                            std::conj(rotm(l1, l3)) * occupancy_number_(jj, kk, ispn, ia, 0) * rotm(l2, l4) * alpha;
                                    }
                                }
                            }
                        }

                        // we have the rotated occupancy matrix
                        for (int ii = 0; ii < lmax_at; ii++) {
                            for (int jj = 0; jj < lmax_at; jj++) {
                                occupancy_number_(ii, jj, ispn, ia, 0) = rot_spa(ii, jj);
                            }
                        }
                        rot_spa.zero();
                    }
                }
            }
        }

        for (int ia = 0; ia < unit_cell_.num_atoms(); ia++) {
            auto& atom = unit_cell_.atom(ia);
            if (atom.type().hubbard_correction()) {
                const int lmax_at = 2 * atom.type().hubbard_orbital(0).hubbard_l() + 1;
                for (int ii = 0; ii < lmax_at; ii++) {
                    for (int ll = 0; ll < lmax_at; ll++) {
                        for (int s = 0; s < ctx_.num_spins(); s++) {
                            this->occupancy_number_(ii, ll, s, ia, 0) = rotated_oc(ii, ll, s, ia);
                        }
                    }
                }
            }
        }
    }
}

/**
 * retrieve or initialize the hubbard occupancies
 *
 * this functions helps retrieving or setting up the hubbard occupancy
 * tensors from an external tensor. Retrieving it is done by specifying
 * "get" in the first argument of the method while setting it is done
 * with the parameter set up to "set". The second parameter is the
 * output pointer and the last parameter is the leading dimension of the
 * tensor.
 *
 * The returned result has the same layout than SIRIUS layout, * i.e.,
 * the harmonic orbitals are stored from m_z = -l..l. The occupancy
 * matrix can also be accessed through the method occupation_matrix()
 *
 *
 * @param what__ string to set to "set" for initializing sirius
 * occupancy tensor and "get" for retrieving it
 * @param pointer to external occupancy tensor
 * @param leading dimension of the outside tensor
 * @return
 * return the occupancy matrix if the first parameter is set to "get"
 */

void Hubbard::access_hubbard_occupancies(char const*     what__,
                                         double_complex* occ__,
                                         int const*      ld__)
{
    std::string what(what__);

    if (!(what == "get" || what == "set")) {
        std::stringstream s;
        s << "wrong access label: " << what;
        TERMINATE(s);
    }

    mdarray<double_complex, 4> occ_mtrx;
    /* in non-collinear case the occupancy matrix is complex */
    if (ctx_.num_mag_dims() == 3) {
        occ_mtrx = mdarray<double_complex, 4>(reinterpret_cast<double_complex*>(occ__), *ld__, *ld__, 4, ctx_.unit_cell().num_atoms());
    } else {
        occ_mtrx = mdarray<double_complex, 4>(reinterpret_cast<double_complex*>(occ__), *ld__, *ld__, ctx_.num_spins(), ctx_.unit_cell().num_atoms());
    }
    if (what == "get") {
        occ_mtrx.zero();
    }

    auto& occupation_matrix = this->occupation_matrix();

    for (int ia = 0; ia < ctx_.unit_cell().num_atoms(); ia++) {
        auto& atom = ctx_.unit_cell().atom(ia);
        if (atom.type().hubbard_correction()) {
            const int l = ctx_.unit_cell().atom(ia).type().hubbard_orbital(0).hubbard_l();
            for (int m1 = -l; m1 <= l; m1++) {
                for (int m2 = -l; m2 <= l; m2++) {
                    if (what == "get") {
                        for (int j = 0; j < ((ctx_.num_mag_dims() == 3) ? 4 : ctx_.num_spins()); j++) {
                            occ_mtrx(l + m1, l + m2, j, ia) = occupation_matrix(l + m1, l + m2, j, ia, 0);
                        }
                    } else {
                        for (int j = 0; j < ((ctx_.num_mag_dims() == 3) ? 4 : ctx_.num_spins()); j++) {
                            occupation_matrix(l + m1, l + m2, j, ia, 0) = occ_mtrx(l + m1, l + m2, j, ia);
                        }
                    }
                }
            }
        }
    }
}
