/*
 * @BEGIN LICENSE
 *
 * Psi4: an open-source quantum chemistry software package
 *
 * Copyright (c) 2007-2019 The Psi4 Developers.
 *
 * The copyrights for code used from other parties are included in
 * the corresponding files.
 *
 * This file is part of Psi4.
 *
 * Psi4 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * Psi4 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along
 * with Psi4; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * @END LICENSE
 */

#include <cmath>
#include <algorithm>

#include "psi4/libmoinfo/libmoinfo.h"
#include "psi4/libpsi4util/libpsi4util.h"

#define CCTRANSFORM_USE_BLAS

#define MAX(i, j) ((i > j) ? i : j)
#define MIN(i, j) ((i > j) ? j : i)
#define INDEX(i, j) ((i > j) ? (ioff[(i)] + (j)) : (ioff[(j)] + (i)))
#define four(i, j, k, l) INDEX(INDEX(i, j), INDEX(k, l))

#include "psi4/libciomr/libciomr.h"
#include "psi4/libpsio/psio.hpp"
#include "psi4/libiwl/iwl.h"
#include "psi4/libqt/qt.h"
#include "psi4/psifiles.h"

#include "algebra_interface.h"
#include "blas.h"
#include "index.h"
#include "matrix.h"
#include "transform.h"

namespace psi {
namespace psimrcc {

CCTransform::CCTransform(std::shared_ptr<PSIMRCCWfn> wfn) : fraction_of_memory_for_presorting(0.75), wfn_(wfn) {
    wfn_->blas()->add_index("[s>=s]");
    wfn_->blas()->add_index("[n>=n]");
    wfn_->blas()->add_index("[s]");
    tei_mo_indexing = wfn_->blas()->get_index("[n>=n]");
    tei_so_indexing = wfn_->blas()->get_index("[s>=s]");
    oei_so_indexing = wfn_->blas()->get_index("[s]");
    first_irrep_in_core = 0;
    last_irrep_in_core = 0;
}

CCTransform::~CCTransform() { free_memory(); }

void CCTransform::read_mo_integrals() {
    read_oei_mo_integrals();
    read_tei_mo_integrals();
}

void CCTransform::read_so_integrals() { read_tei_so_integrals(); }

/*!
    \fn CCTransform::read_tei_integrals()
 */
void CCTransform::read_tei_so_integrals() {
    // Read all the (frozen + non-frozen) TEI in Pitzer order
    // and store them in a in-core block-matrix
    CCIndex* indexing = wfn_->blas()->get_index("[s>=s]");

    // Allocate the tei_so matrix blocks
    tei_so = std::vector<std::vector<double>>(wfn_->nirrep());

    for (int h = 0; h < wfn_->moinfo()->get_nirreps(); h++) {
        if (indexing->get_pairpi(h) > 0) {
            size_t block_size = INDEX(indexing->get_pairpi(h) - 1, indexing->get_pairpi(h) - 1) + 1;
            tei_so[h] = std::vector<double>(block_size, 0);
            outfile->Printf("\n\tCCTransform: allocated the %s block of size %lu", wfn_->moinfo()->get_irr_labs(h).c_str(),
                            block_size);
        }
    }

    double value;
    size_t p, q, r, s, pq, rs, pqrs, irrep;
    int ilsti, nbuf, fi, index, elements;
    elements = 0;
    struct iwlbuf ERIIN;
    iwl_buf_init(&ERIIN, PSIF_SO_TEI, 0.0, 1, 1);
    do {
        ilsti = ERIIN.lastbuf;
        nbuf = ERIIN.inbuf;
        fi = 0;
        for (index = 0; index < nbuf; index++) {
            // Compute the [pq] index for this pqrs combination
            p = abs(ERIIN.labels[fi]);
            q = ERIIN.labels[fi + 1];
            r = ERIIN.labels[fi + 2];
            s = ERIIN.labels[fi + 3];
            value = ERIIN.values[index];
            irrep = indexing->get_tuple_irrep(p, q);
            pq = indexing->get_tuple_rel_index(p, q);
            rs = indexing->get_tuple_rel_index(r, s);
            pqrs = INDEX(pq, rs);
            tei_so[irrep][pqrs] = value;
            fi += 4;
            elements++;
        }
        if (!ilsti) iwl_buf_fetch(&ERIIN);
    } while (!ilsti);

    outfile->Printf("\n    CCTransform: read %d non-zero integrals", elements);
    iwl_buf_close(&ERIIN, 1);

    //   for(int h=0;h<wfn_->moinfo()->get_nirreps();h++){
    //     char label[80];
    //     sprintf(label,"tei_so_%d",h);
    //     psio_write_entry(MRCC_SO_INTS,label,(char*)&tei_so[h][0],INDEX(indexing->get_pairpi(h)-1,indexing->get_pairpi(h)-1)+1*sizeof(double));
    //   }
    //
    //   for(int h=0;h<wfn_->moinfo()->get_nirreps();h++){
    //     if(indexing->get_pairpi(h)>0){
    //       delete[] tei_so[h];
    //     }
    //   }
    //   delete[] tei_so;
}

/*!
    \fn CCTransform::transform_tei_integrals()
 */
void CCTransform::transform_tei_so_integrals() {
    double alpha = 1.0;
    double beta = 0.0;
    int nirreps = wfn_->moinfo()->get_nirreps();
    int pq, rs;
    int p_abs, q_abs, r_abs, s_abs, pqrs;
    int kl, i_abs, j_abs, k_abs, l_abs;
    double** A;
    double** B;
    double** D;
    double** C;
    CCIndex* rsindx = wfn_->blas()->get_index("[s>=s]");
    CCIndex* ijindx = wfn_->blas()->get_index("[n>=n]");
    CCIndex* elemindx = wfn_->blas()->get_index("[s]");

    // First-half transform
    outfile->Printf("\n\tCCTransform: beginning first-half integral trasform");

    for (int h_rs = 0; h_rs < nirreps; h_rs++) {
        for (int h_p = 0; h_p < nirreps; h_p++) {
            int h_q = h_rs ^ h_p;
            if (h_p < h_q) continue;

            int rows_A = elemindx->get_pairpi(h_q);
            int cols_A = elemindx->get_pairpi(h_p);
            int rows_B = wfn_->moinfo()->get_mopi(h_p);
            int cols_B = elemindx->get_pairpi(h_q);
            int rows_D = wfn_->moinfo()->get_mopi(h_q);
            int cols_D = wfn_->moinfo()->get_mopi(h_p);

            if (rows_A * cols_A * rows_B * cols_B * rows_D * cols_D > 0) {
                A = block_matrix(rows_A, cols_A);
                B = block_matrix(rows_B, cols_B);
                D = block_matrix(rows_D, cols_D);
                for (int rs = 0; rs < rsindx->get_pairpi(h_rs); rs++) {
                    zero_arr(&(A[0][0]), rows_A * cols_A);
                    zero_arr(&(B[0][0]), rows_B * cols_B);
                    zero_arr(&(D[0][0]), rows_D * cols_D);

                    // Fill the A matrix
                    for (int q = 0; q < rows_A; q++) {
                        for (int p = 0; p < cols_A; p++) {
                            p_abs = p + elemindx->get_first(h_p);
                            q_abs = q + elemindx->get_first(h_q);
                            p_abs >= q_abs ? (pq = rsindx->get_tuple_rel_index(p_abs, q_abs))
                                           : (pq = rsindx->get_tuple_rel_index(q_abs, p_abs));
                            pqrs = INDEX(pq, rs);
                            A[q][p] = tei_so[h_rs][pqrs];
                        }
                    }

                    // First transform
                    C = wfn_->moinfo()->get_scf_mos(h_p);
// B(i,q)=C(p,i)*A(q,p)
#ifdef CCTRANSFORM_USE_BLAS
                    if (rows_B * cols_B * cols_A != 0)
                        C_DGEMM_12(rows_B, cols_B, cols_A, alpha, &(C[0][0]), cols_A, &(A[0][0]), cols_A, beta,
                                   &(B[0][0]), cols_B);
#else
                    for (int p = 0; p < cols_A; p++)
                        for (int q = 0; q < rows_A; q++)
                            for (int i = 0; i < rows_B; i++) B[i][q] += C[p][i] * A[q][p];
#endif

                    // Second transform
                    C = wfn_->moinfo()->get_scf_mos(h_q);
// D(j,i)+=C(q,j)*B(i,q);
#ifdef CCTRANSFORM_USE_BLAS
                    if (rows_D * cols_D * cols_B != 0)
                        C_DGEMM_12(rows_D, cols_D, cols_B, alpha, &(C[0][0]), cols_B, &(B[0][0]), cols_B, beta,
                                   &(D[0][0]), cols_D);
#else
                    for (int q = 0; q < cols_B; q++)
                        for (int i = 0; i < rows_B; i++)
                            for (int j = 0; j < rows_D; j++) D[j][i] += C[q][j] * B[i][q];
#endif
                    // Store the half-transformed integrals
                    for (int i = 0; i < wfn_->moinfo()->get_mopi(h_p); i++) {
                        for (int j = 0; j < wfn_->moinfo()->get_mopi(h_q); j++) {
                            i_abs = i + elemindx->get_first(h_p);
                            j_abs = j + elemindx->get_first(h_q);
                            if (i_abs >= j_abs) {
                                int ij = ijindx->get_tuple_rel_index(i_abs, j_abs);
                                tei_half_transformed[h_rs][ij][rs] = D[j][i];
                            }
                        }
                    }
                }
                free_block(A);
                free_block(B);
                free_block(D);
            }
        }
    }

    // Second-half transform
    outfile->Printf("\n\tCCTransform: beginning second-half integral transform");

    for (int h_ij = 0; h_ij < nirreps; h_ij++) {
        for (int h_r = 0; h_r < nirreps; h_r++) {
            int h_s = h_ij ^ h_r;
            if (h_r < h_s) continue;

            int rows_A = elemindx->get_pairpi(h_s);
            int cols_A = elemindx->get_pairpi(h_r);
            int rows_B = wfn_->moinfo()->get_mopi(h_r);
            int cols_B = elemindx->get_pairpi(h_s);
            int rows_D = wfn_->moinfo()->get_mopi(h_s);
            int cols_D = wfn_->moinfo()->get_mopi(h_r);

            if (rows_A * cols_A * rows_B * cols_B * rows_D * cols_D > 0) {
                A = block_matrix(rows_A, cols_A);
                B = block_matrix(rows_B, cols_B);
                D = block_matrix(rows_D, cols_D);
                for (int ij = 0; ij < ijindx->get_pairpi(h_ij); ij++) {
                    zero_arr(&(A[0][0]), rows_A * cols_A);
                    zero_arr(&(B[0][0]), rows_B * cols_B);
                    zero_arr(&(D[0][0]), rows_D * cols_D);
                    // Fill the A matrix
                    for (int r = 0; r < elemindx->get_pairpi(h_r); r++) {
                        for (int s = 0; s < elemindx->get_pairpi(h_s); s++) {
                            r_abs = r + elemindx->get_first(h_r);
                            s_abs = s + elemindx->get_first(h_s);
                            r_abs >= s_abs ? (rs = rsindx->get_tuple_rel_index(r_abs, s_abs))
                                           : (rs = rsindx->get_tuple_rel_index(s_abs, r_abs));
                            A[s][r] = tei_half_transformed[h_ij][ij][rs];
                        }
                    }

                    // First transform
                    C = wfn_->moinfo()->get_scf_mos(h_r);
// B(k,s)=C(r,k)*A(s,r)
#ifdef CCTRANSFORM_USE_BLAS
                    if (rows_B * cols_B * cols_A != 0)
                        C_DGEMM_12(rows_B, cols_B, cols_A, alpha, &(C[0][0]), cols_A, &(A[0][0]), cols_A, beta,
                                   &(B[0][0]), cols_B);
#else
                    for (int r = 0; r < cols_A; r++)
                        for (int s = 0; s < rows_A; s++)
                            for (int k = 0; k < rows_B; k++) B[k][s] += C[r][k] * A[s][r];
#endif

                    // Second transform
                    C = wfn_->moinfo()->get_scf_mos(h_s);
// D(l,k)+=C(s,l)*B(k,s);
#ifdef CCTRANSFORM_USE_BLAS
                    if (rows_D * cols_D * cols_B != 0)
                        C_DGEMM_12(rows_D, cols_D, cols_B, alpha, &(C[0][0]), cols_B, &(B[0][0]), cols_B, beta,
                                   &(D[0][0]), cols_D);
#else
                    for (int s = 0; s < cols_B; s++)
                        for (int k = 0; k < rows_B; k++)
                            for (int l = 0; l < rows_D; l++) D[l][k] += C[s][l] * B[k][s];
#endif

                    // Store the half-transformed integrals
                    for (int k = 0; k < wfn_->moinfo()->get_mopi(h_r); k++) {
                        for (int l = 0; l < wfn_->moinfo()->get_mopi(h_s); l++) {
                            k_abs = k + elemindx->get_first(h_r);
                            l_abs = l + elemindx->get_first(h_s);
                            if (k_abs >= l_abs) {
                                kl = ijindx->get_tuple_rel_index(k_abs, l_abs);
                                tei_half_transformed[h_ij][ij][kl] = D[l][k];
                            }
                        }
                    }
                }
                free_block(A);
                free_block(B);
                free_block(D);
            }
        }
    }
    //   for(int h_ij=0;h_ij<nirreps;h_ij++){
    //     for(int ij=ijindx->get_first(h_ij);ij<ijindx->get_last(h_ij);ij++){
    //       ij_tuple = ijindx->get_tuple(ij);
    //       int ij_abs = ij - ijindx->get_first(h_ij);
    //       for(int kl=ijindx->get_first(h_ij);kl<=ij;kl++){
    //         kl_tuple = ijindx->get_tuple(kl);
    //         int kl_abs = kl - ijindx->get_first(h_ij);
    //         outfile->Printf("\n (%2d %2d|%2d %2d) =
    //         %15.10f",ij_tuple[0],ij_tuple[1],kl_tuple[0],kl_tuple[1],tei_half_transformed[h_ij][ij_abs][kl_abs]);
    //       }
    //     }
    //   }
    outfile->Printf("\n\tCCTransform: end of integral transform");
}

/**
 * Read the two electron MO integrals from an iwl buffer assuming Pitzer ordering and store them in the packed array
 * tei_mo
 */
void CCTransform::read_tei_mo_integrals() {
    // Read all the (frozen + non-frozen) TEI in Pitzer order
    // and store them in a in-core block-matrix
    CCIndex* mo_indexing = wfn_->blas()->get_index("[n>=n]");

    allocate_tei_mo();

    double value;
    size_t p, q, r, s, pq, rs, pqrs, irrep;
    size_t ilsti, nbuf, fi, index, elements;
    elements = 0;
    struct iwlbuf ERIIN;
    iwl_buf_init(&ERIIN, PSIF_MO_TEI, 0.0, 1, 1);
    do {
        ilsti = ERIIN.lastbuf;
        nbuf = ERIIN.inbuf;
        fi = 0;
        for (index = 0; index < nbuf; index++) {
            // Compute the [pq] index for this pqrs combination
            p = abs(ERIIN.labels[fi]);
            q = ERIIN.labels[fi + 1];
            r = ERIIN.labels[fi + 2];
            s = ERIIN.labels[fi + 3];
            value = ERIIN.values[index];
            irrep = mo_indexing->get_tuple_irrep(p, q);
            pq = mo_indexing->get_tuple_rel_index(p, q);
            rs = mo_indexing->get_tuple_rel_index(r, s);
            pqrs = INDEX(pq, rs);
            tei_mo[irrep][pqrs] = value;
            fi += 4;
            elements++;
        }
        if (!ilsti) iwl_buf_fetch(&ERIIN);
    } while (!ilsti);
    outfile->Printf("\n    CCTransform: read %lu non-zero integrals", elements);

    iwl_buf_close(&ERIIN, 1);
}

/**
 * Read the one electron MO integrals from an iwl buffer assuming Pitzer ordering and store them in oei_mo
 */
void CCTransform::read_oei_mo_integrals() {
    // Read all the (frozen + non-frozen) OEI in Pitzer order
    allocate_oei_mo();

    int nmo = wfn_->moinfo()->get_nmo();

    std::vector<double> H(INDEX(nmo - 1, nmo - 1) + 1, 0);

    iwl_rdone(PSIF_OEI, const_cast<char*>(PSIF_MO_FZC), H.data(), nmo * (nmo + 1) / 2, 0, 0, "outfile");
    //   else
    //     iwl_rdone(PSIF_OEI,PSIF_MO_FZC,H,norbs*(norbs+1)/2,0,1,outfile); //TODO fix it!

    for (int i = 0; i < nmo; i++)
        for (int j = 0; j < nmo; j++) oei_mo[i][j] = H[INDEX(i, j)];
}

/**
 * Free all the memory allocated by CCTransform
 */
void CCTransform::free_memory() {
    integral_map.clear();
}

/**
 * Allocate the oei_mo array
 */
void CCTransform::allocate_oei_mo() {
    if (oei_mo.size() == 0) {
        int nmo = wfn_->nmo();
        oei_mo = std::vector<std::vector<double>>(nmo, std::vector<double>(nmo, 0));
    }
}

/**
 * Allocate the oei_so array
 */
void CCTransform::allocate_oei_so() {
    if (oei_mo.size() == 0) {
        int nso = wfn_->nso();
        oei_so = std::vector<std::vector<double>>(nso, std::vector<double>(nso, 0));
    }
}

/**
 * Allocate the tei_mo array and exit(EXIT_FAILURE) if there is not enough space
 */
void CCTransform::allocate_tei_mo() {
    if (tei_mo.size() == 0) {
        CCIndex* indexing = wfn_->blas()->get_index("[n>=n]");

        // Allocate the tei_mo matrix blocks
        tei_mo = std::vector<std::vector<double>>(wfn_->nirrep());

        for (int h = 0; h < wfn_->nirrep(); h++) {
            if (indexing->get_pairpi(h) > 0) {
                size_t block_size = INDEX(indexing->get_pairpi(h) - 1, indexing->get_pairpi(h) - 1) + 1;
                tei_mo[h] = std::vector<double>(block_size, 0);
                outfile->Printf("\n\tCCTransform: allocated the %s block of size %lu bytes (free memory = %14lu bytes)",
                                wfn_->moinfo()->get_irr_labs(h).c_str(), block_size, wfn_->free_memory_);
            }
        }
    }
}

void CCTransform::allocate_tei_so() {
    if (tei_so.size() == 0) {
        CCIndex* indexing = wfn_->blas()->get_index("[s>=s]");

        // Allocate the tei_so matrix blocks
        tei_so = std::vector<std::vector<double>>(wfn_->nirrep());

        for (int h = 0; h < wfn_->nirrep(); h++) {
            if (indexing->get_pairpi(h) > 0) {
                int block_size = INDEX(indexing->get_pairpi(h) - 1, indexing->get_pairpi(h) - 1) + 1;
                tei_so[h] = std::vector<double>(block_size, 0);
                outfile->Printf("\n\tCCTransform: allocated the %s block of size %d bytes (free memory = %14lu bytes)",
                                wfn_->moinfo()->get_irr_labs(h).c_str(), block_size, wfn_->free_memory_);
            }
        }
    }
}

void CCTransform::allocate_tei_half_transformed() {
    if (tei_half_transformed.size() == 0) {
        CCIndex* so_indexing = wfn_->blas()->get_index("[s>=s]");
        CCIndex* mo_indexing = wfn_->blas()->get_index("[n>=n]");

        tei_half_transformed = std::vector<std::vector<std::vector<double>>>(wfn_->nirrep());

        bool failed = false;
        size_t required_size = 0;
        size_t matrix_size = 0;

        for (int h = 0; h < wfn_->moinfo()->get_nirreps(); h++) {
            if (so_indexing->get_pairpi(h) * mo_indexing->get_pairpi(h) > 0) {
                tei_half_transformed[h] = std::vector<std::vector<double>>(mo_indexing->get_pairpi(h), std::vector<double>(so_indexing->get_pairpi(h), 0));
                outfile->Printf("\n\tCCTransform: allocated the %s block of size %lu*%lu",
                                wfn_->moinfo()->get_irr_labs(h).c_str(), mo_indexing->get_pairpi(h),
                                so_indexing->get_pairpi(h));
            }
        }
    }
}

double CCTransform::oei(int p, int q) { return (oei_mo[p][q]); }

double CCTransform::tei(int p, int q, int r, int s) {
    // Get the (pq|rs) integral
    return (tei_mo[tei_mo_indexing->get_tuple_irrep(MAX(p, q), MIN(p, q))]
                  [INDEX(tei_mo_indexing->get_tuple_rel_index(MAX(p, q), MIN(p, q)),
                         tei_mo_indexing->get_tuple_rel_index(MAX(r, s), MIN(r, s)))]);
}

/*!
    \fn CCTransform::transform_oei_so_integrals()
 */
void CCTransform::transform_oei_so_integrals() {
    outfile->Printf("\n  CCTransform: transforming one-electron integrals");

    allocate_oei_mo();

    int nso = wfn_->nso();
    int nmo = wfn_->nmo();

    std::vector<std::vector<double>> A(nso, std::vector<double>(nmo, 0));
    auto C = wfn_->moinfo()->get_scf_mos();

    // A(q,i) = H(q,p) * C(p,i)
    /*#ifdef CCTRANSFORM_USE_BLAS
      C_DGEMM_12(
    #else*/
    for (int q = 0; q < nso; q++)
        for (int j = 0; j < nmo; j++) {
            A[q][j] = 0.0;
            for (int p = 0; p < nso; p++) A[q][j] += oei_so[q][p] * C[p][j];
        }
    for (int i = 0; i < nmo; i++)
        for (int j = 0; j < nmo; j++) {
            oei_mo[i][j] = 0.0;
            for (int q = 0; q < nso; q++) oei_mo[i][j] += C[q][i] * A[q][j];
        }
    // #endif
}

}  // namespace psimrcc
}  // namespace psi
