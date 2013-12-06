#ifndef __PLINK_LD_H__
#define __PLINK_LD_H__

#include "plink_common.h"

#define LD_MATRIX_SQ 1
#define LD_MATRIX_SQ0 2
#define LD_MATRIX_TRI 3
#define LD_MATRIX_SHAPEMASK 3
#define LD_MATRIX_BIN 4
#define LD_MATRIX_SPACES 8
#define LD_R2 0x10
#define LD_INTER_CHR 0x20
#define LD_REPORT_GZ 0x40
#define LD_SINGLE_PREC 0x80
#define LD_DPRIME 0x100
#define LD_WITH_FREQS 0x200
#define LD_YES_REALLY 0x400
#define LD_PRUNE_PAIRWISE 0x800
#define LD_IGNORE_X 0x1000
#define LD_WEIGHTED_X 0x2000
#define LD_SNP_LIST_FILE 0x4000

typedef struct {
  uint32_t modifier;
  uint32_t prune_window_size;
  uint32_t prune_window_incr;
  uint32_t prune_window_kb;
  double prune_last_param; // VIF or r^2 threshold
  uint32_t window_size;
  uint32_t window_bp;
  double window_r2;
  char* snpstr;
  Range_list snps_rl;
} Ld_info;

// fast epistasis test is really similar to LD scan so we put it in the same
// place
#define EPI_FAST 1
#define EPI_FAST_CASE_ONLY 2
#define EPI_FAST_NO_UEKI 4
#define EPI_FAST_BOOST 8
#define EPI_FAST_JOINT_EFFECTS 0x10
#define EPI_FAST_NO_P_VALUE 0x20
#define EPI_REG 0x40
#define EPI_SET_BY_SET 0x80
#define EPI_SET_BY_ALL 0x100

typedef struct {
  uint32_t modifier;
  uint32_t case_only_gap;
  double epi1;
  double epi2;
  // twolocus() handles --ld too
  char* ld_mkr1;
  char* ld_mkr2;
  char* twolocus_mkr1;
  char* twolocus_mkr2;
  char* summary_merge_prefix;
  uint32_t summary_merge_ct;
} Epi_info;

void ld_epi_init(Ld_info* ldip, Epi_info* epi_ip);

void ld_epi_cleanup(Ld_info* ldip, Epi_info* epi_ip);

int32_t ld_prune(Ld_info* ldip, FILE* bedfile, uintptr_t bed_offset, uintptr_t marker_ct, uintptr_t unfiltered_marker_ct, uintptr_t* marker_exclude, uintptr_t* marker_reverse, char* marker_ids, uintptr_t max_marker_id_len, Chrom_info* chrom_info_ptr, double* set_allele_freqs, uint32_t* marker_pos, uintptr_t unfiltered_indiv_ct, uintptr_t* founder_info, uintptr_t* sex_male, char* outname, char* outname_end, uint32_t hh_exists);

int32_t ld_report(pthread_t* threads, Ld_info* ldip, FILE* bedfile, uintptr_t bed_offset, uintptr_t marker_ct, uintptr_t unfiltered_marker_ct, uintptr_t* marker_exclude, uintptr_t* marker_reverse, char* marker_ids, uintptr_t max_marker_id_len, uint32_t plink_maxsnp, char** marker_allele_ptrs, uintptr_t max_marker_allele_len, double* set_allele_freqs, uint32_t zero_extra_chroms, Chrom_info* chrom_info_ptr, uint32_t* marker_pos, uintptr_t unfiltered_indiv_ct, uintptr_t* founder_info, uint32_t parallel_idx, uint32_t parallel_tot, uintptr_t* sex_male, char* outname, char* outname_end, uint32_t hh_exists);

int32_t twolocus(Epi_info* epi_ip, FILE* bedfile, uintptr_t bed_offset, uintptr_t marker_ct, uintptr_t unfiltered_marker_ct, uintptr_t* marker_exclude, uintptr_t* marker_reverse, char* marker_ids, uintptr_t max_marker_id_len, uint32_t plink_maxsnp, char** marker_allele_ptrs, Chrom_info* chrom_info_ptr, uintptr_t unfiltered_indiv_ct, uintptr_t* indiv_exclude, uintptr_t indiv_ct, uintptr_t* pheno_nm, uint32_t pheno_nm_ct, uint32_t pheno_ctrl_ct, uintptr_t* pheno_c, uintptr_t* sex_male, char* outname, char* outname_end, uint32_t hh_exists);

int32_t epistasis_report(pthread_t* threads, Epi_info* epi_ip, FILE* bedfile, uintptr_t bed_offset, uintptr_t marker_ct, uintptr_t unfiltered_marker_ct, uintptr_t* marker_exclude, uintptr_t* marker_reverse, char* marker_ids, uintptr_t max_marker_id_len, uint32_t* marker_pos, uint32_t plink_maxsnp, uint32_t zero_extra_chroms, Chrom_info* chrom_info_ptr, uintptr_t unfiltered_indiv_ct, uintptr_t* pheno_nm, uint32_t pheno_nm_ct, uint32_t ctrl_ct, uintptr_t* pheno_c, double* pheno_d, uint32_t parallel_idx, uint32_t parallel_tot, char* outname, char* outname_end);

int32_t epi_summary_merge(Epi_info* epi_ip, char* outname, char* outname_end);

#endif // __PLINK_LD_H__