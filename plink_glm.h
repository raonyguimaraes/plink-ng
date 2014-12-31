#ifndef __PLINK_GLM_H__
#define __PLINK_GLM_H__

#include "plink_matrix.h"
#include "plink_set.h"

#ifndef NOLAPACK
uint32_t glm_linear(uintptr_t cur_batch_size, uintptr_t param_ct, uintptr_t sample_valid_ct, uint32_t missing_ct, uintptr_t* loadbuf, uint32_t standard_beta, double pheno_sum_base, double pheno_ssq_base, double* covars_cov_major, double* covars_sample_major, double* perm_pmajor, double* coef, double* param_2d_buf, MATRIX_INVERT_BUF1_TYPE* mi_buf, double* param_2d_buf2, double* linear_results, uintptr_t constraint_ct, double* constraints_con_major, double* param_df_buf, double* param_df_buf2, double* df_df_buf, double* df_buf, uint32_t* perm_fail_ct_ptr, uintptr_t* perm_fails);
#endif

uint32_t glm_logistic(uintptr_t cur_batch_size, uintptr_t param_ct, uintptr_t sample_valid_ct, uint32_t missing_ct, uintptr_t* loadbuf, float* covars_cov_major, uintptr_t* perm_vecs, float* coef, float* pp, float* sample_1d_buf, float* pheno_buf, float* param_1d_buf, float* param_1d_buf2, float* param_2d_buf, float* param_2d_buf2, float* logistic_results, uintptr_t constraint_ct, double* constraints_con_major, double* param_1d_dbuf, double* param_2d_dbuf, double* param_2d_dbuf2, double* param_df_dbuf, double* df_df_dbuf, MATRIX_INVERT_BUF1_TYPE* mi_buf, double* df_dbuf, uintptr_t* perm_fails);

#ifndef NOLAPACK
int32_t glm_linear_assoc(pthread_t* threads, FILE* bedfile, uintptr_t bed_offset, char* outname, char* outname_end, uint32_t glm_modifier, double glm_vif_thresh, uint32_t glm_xchr_model, uint32_t glm_mperm_val, Range_list* parameters_range_list_ptr, Range_list* tests_range_list_ptr, double ci_size, double ci_zt, double pfilter, double output_min_p, uint32_t mtest_adjust, double adjust_lambda, uintptr_t unfiltered_marker_ct, uintptr_t* marker_exclude_orig, uintptr_t marker_ct, char* marker_ids, uintptr_t max_marker_id_len, uint32_t plink_maxsnp, uint32_t* marker_pos, char** marker_allele_ptrs, uintptr_t max_marker_allele_len, uintptr_t* marker_reverse, char* condition_mname, char* condition_fname, Chrom_info* chrom_info_ptr, uintptr_t unfiltered_sample_ct, uintptr_t sample_ct, uintptr_t* sample_exclude, uint32_t cluster_ct, uint32_t* cluster_map, uint32_t* cluster_starts, Aperm_info* apip, uint32_t mperm_save, uint32_t pheno_nm_ct, uintptr_t* pheno_nm, double* pheno_d, uintptr_t covar_ct, char* covar_names, uintptr_t max_covar_name_len, uintptr_t* covar_nm, double* covar_d, uintptr_t* founder_info, uintptr_t* sex_nm, uintptr_t* sex_male, uint32_t ld_ignore_x, uint32_t hh_exists, uint32_t orig_perm_batch_size, Set_info* sip);
#endif

int32_t glm_logistic_assoc(pthread_t* threads, FILE* bedfile, uintptr_t bed_offset, char* outname, char* outname_end, uint32_t glm_modifier, double glm_vif_thresh, uint32_t glm_xchr_model, uint32_t glm_mperm_val, Range_list* parameters_range_list_ptr, Range_list* tests_range_list_ptr, double ci_size, double ci_zt, double pfilter, double output_min_p, uint32_t mtest_adjust, double adjust_lambda, uintptr_t unfiltered_marker_ct, uintptr_t* marker_exclude_orig, uintptr_t marker_ct_orig, char* marker_ids, uintptr_t max_marker_id_len, uint32_t plink_maxsnp, uint32_t* marker_pos, char** marker_allele_ptrs, uintptr_t max_marker_allele_len, uintptr_t* marker_reverse, char* condition_mname, char* condition_fname, Chrom_info* chrom_info_ptr, uintptr_t unfiltered_sample_ct, uintptr_t sample_ct, uintptr_t* sample_exclude, uint32_t cluster_ct, uint32_t* cluster_map, uint32_t* cluster_starts, Aperm_info* apip, uint32_t mperm_save, uint32_t pheno_nm_ct, uintptr_t* pheno_nm, uintptr_t* pheno_c, uintptr_t covar_ct, char* covar_names, uintptr_t max_covar_name_len, uintptr_t* covar_nm, double* covar_d, uintptr_t* founder_info, uintptr_t* sex_nm, uintptr_t* sex_male, uint32_t ld_ignore_x, uint32_t hh_exists, uint32_t perm_batch_size, Set_info* sip);

#ifndef NOLAPACK
int32_t glm_linear_nosnp(pthread_t* threads, FILE* bedfile, uintptr_t bed_offset, char* outname, char* outname_end, uint32_t glm_modifier, double glm_vif_thresh, uint32_t glm_xchr_model, uint32_t glm_mperm_val, Range_list* parameters_range_list_ptr, Range_list* tests_range_list_ptr, double ci_size, double ci_zt, double pfilter, double output_min_p, uintptr_t unfiltered_marker_ct, uintptr_t* marker_exclude, uintptr_t marker_ct, char* marker_ids, uintptr_t max_marker_id_len, uintptr_t* marker_reverse, char* condition_mname, char* condition_fname, Chrom_info* chrom_info_ptr, uintptr_t unfiltered_sample_ct, uintptr_t sample_ct, uintptr_t* sample_exclude, uint32_t cluster_ct, uint32_t* cluster_map, uint32_t* cluster_starts, uint32_t mperm_save, uint32_t pheno_nm_ct, uintptr_t* pheno_nm, double* pheno_d, uintptr_t covar_ct, char* covar_names, uintptr_t max_covar_name_len, uintptr_t* covar_nm, double* covar_d, uintptr_t* sex_nm, uintptr_t* sex_male, uint32_t hh_or_mt_exists, uint32_t perm_batch_size, Set_info* sip);
#endif

int32_t glm_logistic_nosnp(pthread_t* threads, FILE* bedfile, uintptr_t bed_offset, char* outname, char* outname_end, uint32_t glm_modifier, double glm_vif_thresh, uint32_t glm_xchr_model, uint32_t glm_mperm_val, Range_list* parameters_range_list_ptr, Range_list* tests_range_list_ptr, double ci_size, double ci_zt, double pfilter, double output_min_p, uintptr_t unfiltered_marker_ct, uintptr_t* marker_exclude, uintptr_t marker_ct, char* marker_ids, uintptr_t max_marker_id_len, uintptr_t* marker_reverse, char* condition_mname, char* condition_fname, Chrom_info* chrom_info_ptr, uintptr_t unfiltered_sample_ct, uintptr_t sample_ct, uintptr_t* sample_exclude, uint32_t cluster_ct, uint32_t* cluster_map, uint32_t* cluster_starts, uint32_t mperm_save, uint32_t pheno_nm_ct, uintptr_t* pheno_nm, uintptr_t* pheno_c, uintptr_t covar_ct, char* covar_names, uintptr_t max_covar_name_len, uintptr_t* covar_nm, double* covar_d, uintptr_t* sex_nm, uintptr_t* sex_male, uint32_t hh_or_mt_exists, uint32_t perm_batch_size, Set_info* sip);

#ifndef NOLAPACK
uint32_t glm_linear_dosage(uintptr_t sample_ct, uintptr_t* cur_samples, uintptr_t sample_valid_ct, uintptr_t* pheno_nm, double* pheno_d, uintptr_t* perm_fails, uintptr_t covar_ct, double* covar_d, double* cur_dosages, double* pheno_d2, double* covars_cov_major, double* covars_sample_major, double* param_2d_buf, MATRIX_INVERT_BUF1_TYPE* mi_buf, double* param_2d_buf2, double* regression_results, double* dgels_a, double* dgels_b, double* dgels_work, __CLPK_integer dgels_lwork, uint32_t standard_beta, double vif_thresh, double* beta_ptr, double* se_ptr, double* pval_ptr);
#endif

uint32_t glm_logistic_dosage(uintptr_t sample_ct, uintptr_t* cur_samples, uintptr_t sample_valid_ct, uintptr_t* pheno_nm, uintptr_t* pheno_c, uintptr_t* perm_vec, uintptr_t* perm_fails, uintptr_t covar_ct, float* covar_f, double* cur_dosages, float* coef, float* pp, float* pheno_buf, float* covars_cov_major, float* param_1d_buf, float* param_1d_buf2, float* param_2d_buf, float* param_2d_buf2, float* regression_results, float* sample_1d_buf, double* beta_ptr, double* se_ptr, double* pval_ptr);

#endif // __PLINK_GLM_H__
