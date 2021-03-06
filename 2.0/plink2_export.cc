// This file is part of PLINK 2.00, copyright (C) 2005-2018 Shaun Purcell,
// Christopher Chang.
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "plink2_compress_stream.h"
#include "plink2_export.h"

#include "htslib/htslib/bgzf.h"
#include "libdeflate/libdeflate.h"

#include <time.h>

#ifdef __cplusplus
namespace plink2 {
#endif

// a few multithread globals
static uint32_t g_sample_ct = 0;
static uint32_t g_calc_thread_ct = 0;
static uint32_t g_cur_block_write_ct = 0;
static PglErr g_error_ret = kPglRetSuccess;

// more multithread globals
static PgenReader** g_pgr_ptrs = nullptr;
static uintptr_t** g_genovecs = nullptr;
static uintptr_t** g_dosage_presents = nullptr;
static Dosage** g_dosage_mains = nullptr;
static uint32_t* g_read_variant_uidx_starts = nullptr;  // size calc_thread_ct

static unsigned char* g_writebufs[2] = {nullptr, nullptr};

static const uintptr_t* g_variant_include = nullptr;
static const ChrInfo* g_cip = nullptr;
static const uintptr_t* g_sample_include = nullptr;
static uint32_t* g_sample_include_cumulative_popcounts = nullptr;
static const uintptr_t* g_variant_allele_idxs = nullptr;
static const AltAlleleCt* g_refalt1_select = nullptr;

static VecW** g_thread_vecaligned_bufs = nullptr;
static uintptr_t** g_thread_write_genovecs = nullptr;
static uintptr_t** g_thread_write_dosagepresents = nullptr;
static Dosage** g_thread_write_dosagevals = nullptr;

static uint32_t g_stride = 0;


// assumes rawval is in [0, 163839]
static_assert(kDosageMid == 16384, "print_small_dosage() needs to be updated.");
char* PrintSmallDosage(uint32_t rawval, char* start) {
  // Instead of constant 5-digit precision, we print fewer digits whenever that
  // doesn't interfere with proper round-tripping.  I.e. we search for the
  // shortest string in
  //   ((n - 0.5)/16384, (n + 0.5)/16384).
  // E.g. 3277/16384 is 0.20001 when printed with 5-digit precision, but we'd
  // print that as 0.2 since that's still in (3276.5/16384, 3277.5/16384).
  *start++ = '0' + (rawval / 16384);
  rawval = rawval % 16384;
  if (!rawval) {
    return start;
  }
  *start++ = '.';
  // (rawval * 2) is in 32768ths
  // 32768 * 625 = 20480k

  const uint32_t range_top_20480k = (rawval * 2 + 1) * 625;
  // this is technically checking a half-open rather than a fully-open
  // interval, but that's fine since we never hit the boundary points
  if ((range_top_20480k % 2048) < 1250) {
    // when this is true, the four-decimal-place approximation is in the range
    // which round-trips back to our original number.
    const uint32_t four_decimal_places = range_top_20480k / 2048;
    return u32toa_trunc4(four_decimal_places, start);
  }

  // we wish to print (100000 * remainder + 8192) / 16384, left-0-padded.  and
  // may as well banker's round too.
  //
  // banker's rounding yields a different result than regular rounding for n/64
  // when n is congruent to 1 mod 4:
  //   1/64 = .015625 -> print 0.01562
  //   3/64 = .046875 -> print 0.04688
  //   5/64 = .078125 -> print 0.07812
  const uint32_t five_decimal_places = ((3125 * rawval + 256) / 512) - ((rawval % 1024) == 256);
  const uint32_t first_decimal_place = five_decimal_places / 10000;
  *start++ = '0' + first_decimal_place;
  const uint32_t last_four_digits = five_decimal_places - first_decimal_place * 10000;
  if (last_four_digits) {
    return u32toa_trunc4(last_four_digits, start);
  }
  return start;
}

#ifdef __arm__
#  error "Unaligned accesses in Export012Vmaj()."
#endif
PglErr Export012Vmaj(const char* outname, const uintptr_t* sample_include, const uint32_t* sample_include_cumulative_popcounts, const char* sample_ids, const uintptr_t* variant_include, const ChrInfo* cip, const uint32_t* variant_bps, const char* const* variant_ids, const uintptr_t* variant_allele_idxs, const char* const* allele_storage, const AltAlleleCt* refalt1_select, const double* variant_cms, uint32_t sample_ct, uintptr_t max_sample_id_blen, uint32_t variant_ct, uint32_t max_allele_slen, char exportf_delim, PgenReader* simple_pgrp) {
  // todo: --recode-allele?
  unsigned char* bigstack_mark = g_bigstack_base;
  FILE* outfile = nullptr;
  PglErr reterr = kPglRetSuccess;
  {
    const uint32_t sample_ctl2 = QuaterCtToWordCt(sample_ct);
    const uint32_t sample_ctl = BitCtToWordCt(sample_ct);
    const uint32_t max_chr_blen = 1 + GetMaxChrSlen(cip);
    char* chr_buf;  // includes trailing tab
    char* writebuf;
    uintptr_t* genovec;
    // dosages are limited to 7 characters (x.yyyyy)
    if (bigstack_alloc_c(max_chr_blen, &chr_buf) ||
        bigstack_alloc_c(kMaxMediumLine + max_chr_blen + 2 * kMaxIdSlen + 48 + 2 * max_allele_slen + (8 * k1LU) * sample_ct, &writebuf) ||
        bigstack_alloc_w(sample_ctl2, &genovec)) {
      goto Export012Vmaj_ret_NOMEM;
    }
    char* writebuf_flush = &(writebuf[kMaxMediumLine]);
    const uint32_t dosage_is_present = simple_pgrp->fi.gflags & kfPgenGlobalDosagePresent;
    uintptr_t* dosage_present = nullptr;
    Dosage* dosage_main = nullptr;
    if (dosage_is_present) {
      if (bigstack_alloc_w(sample_ctl, &dosage_present) ||
          bigstack_alloc_dosage(sample_ct, &dosage_main)) {
        goto Export012Vmaj_ret_NOMEM;
      }
    }
    if (fopen_checked(outname, FOPEN_WB, &outfile)) {
      goto Export012Vmaj_ret_OPEN_FAIL;
    }
    char* write_iter = strcpya(writebuf, (exportf_delim == '\t')? "CHR\tSNP\t(C)M\tPOS\tCOUNTED\tALT" : "CHR SNP (C)M POS COUNTED ALT");
    uint32_t sample_uidx = 0;
    for (uint32_t sample_idx = 0; sample_idx < sample_ct; ++sample_idx, ++sample_uidx) {
      MovU32To1Bit(sample_include, &sample_uidx);
      *write_iter++ = exportf_delim;
      const char* fid_start = &(sample_ids[sample_uidx * max_sample_id_blen]);
      const char* fid_end = AdvToDelim(fid_start, exportf_delim);
      write_iter = memcpyax(write_iter, fid_start, fid_end - fid_start, '_');
      write_iter = strcpya(write_iter, &(fid_end[1]));
      if (fwrite_ck(writebuf_flush, outfile, &write_iter)) {
        goto Export012Vmaj_ret_WRITE_FAIL;
      }
    }
    AppendBinaryEoln(&write_iter);
    logprintfww5("--export A-transpose to %s ... ", outname);
    fputs("0%", stdout);
    fflush(stdout);
    uint32_t chr_fo_idx = UINT32_MAX;
    uint32_t chr_end = 0;
    uint32_t chr_blen = 0;
    uint32_t ref_allele_idx = 0;
    uint32_t cur_allele_ct = 2;
    const uint32_t sample_ctl2_m1 = sample_ctl2 - 1;

    uint32_t pct = 0;
    uint32_t next_print_variant_idx = variant_ct / 100;
    uint32_t variant_uidx = 0;
    for (uint32_t variant_idx = 0; variant_idx < variant_ct; ++variant_idx, ++variant_uidx) {
      MovU32To1Bit(variant_include, &variant_uidx);
      if (variant_uidx >= chr_end) {
        do {
          ++chr_fo_idx;
          chr_end = cip->chr_fo_vidx_start[chr_fo_idx + 1];
        } while (variant_uidx >= chr_end);
        const uint32_t chr_idx = cip->chr_file_order[chr_fo_idx];
        char* chr_name_end = chrtoa(cip, chr_idx, chr_buf);
        *chr_name_end++ = exportf_delim;
        chr_blen = chr_name_end - chr_buf;
      }
      write_iter = memcpya(write_iter, chr_buf, chr_blen);
      write_iter = strcpyax(write_iter, variant_ids[variant_uidx], exportf_delim);
      if (variant_cms) {
        write_iter = dtoa_g(variant_cms[variant_uidx], write_iter);
      } else {
        *write_iter++ = '0';
      }
      *write_iter++ = exportf_delim;
      write_iter = u32toa_x(variant_bps[variant_uidx], exportf_delim, write_iter);
      // todo: multiallelic case
      uint32_t dosage_ct;
      reterr = PgrGetD(sample_include, sample_include_cumulative_popcounts, sample_ct, variant_uidx, simple_pgrp, genovec, dosage_present, dosage_main, &dosage_ct);
      if (reterr) {
        if (reterr != kPglRetReadFail) {
          logputs("\n");
          logerrputs("Error: Malformed .pgen file.\n");
        }
        goto Export012Vmaj_ret_1;
      }
      if (refalt1_select) {
        ref_allele_idx = refalt1_select[2 * variant_uidx];
      }
      if (!ref_allele_idx) {
        // we *usually* invert, since COUNTED = REF.
        GenovecInvertUnsafe(sample_ct, genovec);
        BiallelicDosage16Invert(dosage_ct, dosage_main);
      }
      uintptr_t variant_allele_idx_base = variant_uidx * 2;
      if (variant_allele_idxs) {
        variant_allele_idx_base = variant_allele_idxs[variant_uidx];
      }
      const char* const* cur_alleles = &(allele_storage[variant_allele_idx_base]);
      write_iter = strcpyax(write_iter, cur_alleles[ref_allele_idx], exportf_delim);
      const uint32_t first_alt_idx = (ref_allele_idx == 0);
      write_iter = strcpya(write_iter, cur_alleles[first_alt_idx]);
      if (cur_allele_ct > 2) {
        for (uint32_t allele_idx = first_alt_idx + 1; allele_idx < cur_allele_ct; ++allele_idx) {
          if (allele_idx == ref_allele_idx) {
            continue;
          }
          if (fwrite_ck(writebuf_flush, outfile, &write_iter)) {
            goto Export012Vmaj_ret_WRITE_FAIL;
          }
          *write_iter++ = ',';
          write_iter = strcpya(write_iter, cur_alleles[allele_idx]);
        }
      }
      if (fwrite_ck(writebuf_flush, outfile, &write_iter)) {
        goto Export012Vmaj_ret_WRITE_FAIL;
      }
      uint32_t widx = 0;
      uint32_t loop_len = kBitsPerWordD2;
      if (!dosage_ct) {
        while (1) {
          if (widx >= sample_ctl2_m1) {
            if (widx > sample_ctl2_m1) {
              break;
            }
            loop_len = ModNz(sample_ct, kBitsPerWordD2);
          }
          uintptr_t geno_word = genovec[widx];
          for (uint32_t sample_idx_lowbits = 0; sample_idx_lowbits < loop_len; ++sample_idx_lowbits) {
            *write_iter++ = exportf_delim;
            uintptr_t cur_geno = geno_word & 3;
            if (cur_geno != 3) {
              *write_iter++ = '0' + cur_geno;
            } else {
              write_iter = strcpya(write_iter, "NA");
            }
            geno_word >>= 2;
          }
          ++widx;
        }
      } else {
        Dosage* dosage_main_iter = dosage_main;
        while (1) {
          if (widx >= sample_ctl2_m1) {
            if (widx > sample_ctl2_m1) {
              break;
            }
            loop_len = ModNz(sample_ct, kBitsPerWordD2);
          }
          uintptr_t geno_word = genovec[widx];
          uint32_t dosage_present_hw = R_CAST(Halfword*, dosage_present)[widx];
          for (uint32_t sample_idx_lowbits = 0; sample_idx_lowbits < loop_len; ++sample_idx_lowbits) {
            *write_iter++ = exportf_delim;
            if (dosage_present_hw & 1) {
              write_iter = PrintSmallDosage(*dosage_main_iter++, write_iter);
            } else {
              uintptr_t cur_geno = geno_word & 3;
              if (cur_geno != 3) {
                *write_iter++ = '0' + cur_geno;
              } else {
                write_iter = strcpya(write_iter, "NA");
              }
            }
            geno_word >>= 2;
            dosage_present_hw >>= 1;
          }
          ++widx;
        }
      }
      AppendBinaryEoln(&write_iter);
      if (variant_idx >= next_print_variant_idx) {
        if (pct > 10) {
          putc_unlocked('\b', stdout);
        }
        pct = (variant_idx * 100LLU) / variant_ct;
        printf("\b\b%u%%", pct++);
        fflush(stdout);
        next_print_variant_idx = (pct * S_CAST(uint64_t, variant_ct)) / 100;
      }
    }
    if (fclose_flush_null(writebuf_flush, write_iter, &outfile)) {
      goto Export012Vmaj_ret_WRITE_FAIL;
    }
    if (pct > 10) {
      putc_unlocked('\b', stdout);
    }
    fputs("\b\b", stdout);
    logprintf("done.\n");
  }
  while (0) {
  Export012Vmaj_ret_NOMEM:
    reterr = kPglRetNomem;
    break;
  Export012Vmaj_ret_OPEN_FAIL:
    reterr = kPglRetOpenFail;
    break;
  Export012Vmaj_ret_WRITE_FAIL:
    reterr = kPglRetWriteFail;
    break;
  }
 Export012Vmaj_ret_1:
  fclose_cond(outfile);
  BigstackReset(bigstack_mark);
  return reterr;
}

// more multithread globals
static uintptr_t* g_vmaj_readbuf = nullptr;

THREAD_FUNC_DECL TransposeToSmajReadThread(void* arg) {
  const uintptr_t tidx = R_CAST(uintptr_t, arg);
  PgenReader* pgrp = g_pgr_ptrs[tidx];
  const uintptr_t* variant_include = g_variant_include;
  // const uintptr_t* variant_allele_idxs = g_variant_allele_idxs;
  const AltAlleleCt* refalt1_select = g_refalt1_select;
  const uint32_t calc_thread_ct = g_calc_thread_ct;
  const uintptr_t* sample_include = g_sample_include;
  const uint32_t* sample_include_cumulative_popcounts = g_sample_include_cumulative_popcounts;
  const uint32_t read_sample_ct = g_sample_ct;
  const uintptr_t read_sample_ctaw2 = QuaterCtToAlignedWordCt(read_sample_ct);
  uintptr_t prev_copy_ct = 0;
  while (1) {
    const uint32_t is_last_block = g_is_last_thread_block;
    const uintptr_t cur_block_copy_ct = g_cur_block_write_ct;
    const uint32_t cur_idx_end = ((tidx + 1) * cur_block_copy_ct) / calc_thread_ct;
    uint32_t variant_uidx = g_read_variant_uidx_starts[tidx];
    uint32_t cur_idx = (tidx * cur_block_copy_ct) / calc_thread_ct;
    uintptr_t* vmaj_readbuf_iter = &(g_vmaj_readbuf[(prev_copy_ct + cur_idx) * read_sample_ctaw2]);
    for (; cur_idx < cur_idx_end; ++cur_idx, ++variant_uidx) {
      MovU32To1Bit(variant_include, &variant_uidx);
      // todo: multiallelic case
      const PglErr reterr = PgrGet(sample_include, sample_include_cumulative_popcounts, read_sample_ct, variant_uidx, pgrp, vmaj_readbuf_iter);
      if (reterr) {
        g_error_ret = reterr;
        break;
      }
      if (refalt1_select && (refalt1_select[2 * variant_uidx] == 1)) {
        GenovecInvertUnsafe(read_sample_ct, vmaj_readbuf_iter);
        // don't need ZeroTrailingQuaters()
      }
      vmaj_readbuf_iter = &(vmaj_readbuf_iter[read_sample_ctaw2]);
    }
    if (is_last_block) {
      THREAD_RETURN;
    }
    prev_copy_ct += cur_block_copy_ct;
    THREAD_BLOCK_FINISH(tidx);
  }
}

// more multithread globals
static uintptr_t* g_smaj_writebufs[2] = {nullptr, nullptr};
static uint32_t g_variant_ct = 0;
static uint32_t g_sample_batch_size = 0;
static uint32_t g_output_calc_thread_ct = 0;

THREAD_FUNC_DECL TransposeToPlink1SmajWriteThread(void* arg) {
  const uintptr_t tidx = R_CAST(uintptr_t, arg);
  const uint32_t variant_ct = g_variant_ct;
  const uintptr_t variant_batch_ct = DivUp(variant_ct, kPglQuaterTransposeBatch);
  const uintptr_t variant_batch_word_ct = variant_batch_ct * kPglQuaterTransposeWords;
  const uint32_t calc_thread_ct = g_output_calc_thread_ct;
  const uint32_t variant_batch_idx_start = (S_CAST(uint64_t, tidx) * variant_batch_ct) / calc_thread_ct;
  VecW* vecaligned_buf = g_thread_vecaligned_bufs[tidx];
  uintptr_t variant_batch_idx_full_end = ((S_CAST(uint64_t, tidx) + 1) * variant_batch_ct) / calc_thread_ct;
  uint32_t variant_idx_end;
  if (tidx + 1 < calc_thread_ct) {
    variant_idx_end = variant_batch_idx_full_end * kPglQuaterTransposeBatch;
  } else {
    variant_idx_end = variant_ct;
    if (variant_ct % kPglQuaterTransposeBatch) {
      --variant_batch_idx_full_end;
    }
  }
  const uint32_t thread_variant_ct = variant_idx_end - variant_batch_idx_start * kPglQuaterTransposeBatch;
  const uint32_t read_sample_ct = g_sample_ct;
  const uintptr_t read_sample_ctaw2 = QuaterCtToAlignedWordCt(read_sample_ct);
  const uintptr_t* vmaj_readbuf = g_vmaj_readbuf;
  uint32_t sample_widx = 0;
  uint32_t parity = 0;
  while (1) {
    const uint32_t is_last_block = g_is_last_thread_block;
    uintptr_t variant_batch_idx = variant_batch_idx_start;
    uint32_t variant_batch_size = kPglQuaterTransposeBatch;
    const uintptr_t* vmaj_readbuf_iter = &(vmaj_readbuf[variant_batch_idx * kPglQuaterTransposeBatch * read_sample_ctaw2 + sample_widx]);
    const uint32_t sample_batch_size = g_sample_batch_size;
    uintptr_t* smaj_writebuf_start = &(g_smaj_writebufs[parity][variant_batch_idx * kPglQuaterTransposeWords]);
    uintptr_t* smaj_writebuf_iter = smaj_writebuf_start;
    while (1) {
      if (variant_batch_idx >= variant_batch_idx_full_end) {
        if (variant_batch_idx * kPglQuaterTransposeBatch >= variant_idx_end) {
          break;
        }
        variant_batch_size = variant_idx_end - variant_batch_idx * kPglQuaterTransposeBatch;
      }
      TransposeQuaterblock(vmaj_readbuf_iter, read_sample_ctaw2, variant_batch_word_ct, variant_batch_size, sample_batch_size, smaj_writebuf_iter, vecaligned_buf);
      smaj_writebuf_iter = &(smaj_writebuf_iter[kPglQuaterTransposeWords]);
      vmaj_readbuf_iter = &(vmaj_readbuf_iter[variant_batch_size * read_sample_ctaw2]);
      ++variant_batch_idx;
    }
    smaj_writebuf_iter = smaj_writebuf_start;
    for (uint32_t sample_idx = 0; sample_idx < sample_batch_size; ++sample_idx) {
      // could fold this into TransposeQuaterblock(), but I won't bother,
      // we're already saturating at ~3 threads
      PgrPlink2ToPlink1InplaceUnsafe(thread_variant_ct, smaj_writebuf_iter);
      ZeroTrailingQuaters(thread_variant_ct, smaj_writebuf_iter);
      smaj_writebuf_iter = &(smaj_writebuf_iter[variant_batch_word_ct]);
    }
    if (is_last_block) {
      THREAD_RETURN;
    }
    THREAD_BLOCK_FINISH(tidx);
    parity = 1 - parity;
    sample_widx += sample_batch_size / kBitsPerWordD2;
  }
}

PglErr ExportIndMajorBed(const uintptr_t* orig_sample_include, const uintptr_t* variant_include, const uintptr_t* variant_allele_idxs, const AltAlleleCt* refalt1_select, uint32_t raw_sample_ct, uint32_t sample_ct, uint32_t raw_variant_ct, uint32_t variant_ct, uint32_t max_thread_ct, uintptr_t pgr_alloc_cacheline_ct, PgenFileInfo* pgfip, char* outname, char* outname_end) {
  unsigned char* bigstack_mark = g_bigstack_base;
  FILE* outfile = nullptr;
  PglErr reterr = kPglRetSuccess;
  {
    // Possible special case: if the input file is a variant-major .bed, we do
    // not have enough memory to just load the whole file at once, and there
    // are more than ~20k samples, there can be a performance advantage to not
    // loading an entire variant at a time; we can use smaller fread calls and
    // reduce the number of (typically 4096 byte) disk blocks which need to be
    // read on each pass.  But let's get .pgen -> sample-major humming first.
    snprintf(outname_end, kMaxOutfnameExtBlen, ".bed");
    if (fopen_checked(outname, FOPEN_WB, &outfile)) {
      goto ExportIndMajorBed_ret_OPEN_FAIL;
    }
    if (fwrite_checked("l\x1b\0", 3, outfile)) {
      goto ExportIndMajorBed_ret_WRITE_FAIL;
    }
    if (variant_ct && sample_ct) {
      const uint32_t raw_sample_ctl = BitCtToWordCt(raw_sample_ct);
      uint32_t calc_thread_ct = (max_thread_ct > 2)? (max_thread_ct - 1) : max_thread_ct;
      // todo: if only 1 pass is needed, and no subsetting is happening, this
      // saturates at ~4 threads?
      unsigned char* main_loadbufs[2];
      pthread_t* threads;
      uint32_t read_block_size;
      // note that this is restricted to half of available workspace
      if (PgenMtLoadInit(variant_include, sample_ct, variant_ct, bigstack_left() / 2, pgr_alloc_cacheline_ct, 0, 0, pgfip, &calc_thread_ct, &g_genovecs, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &read_block_size, main_loadbufs, &threads, &g_pgr_ptrs, &g_read_variant_uidx_starts)) {
        goto ExportIndMajorBed_ret_NOMEM;
      }
      g_variant_include = variant_include;
      g_variant_allele_idxs = variant_allele_idxs;
      g_refalt1_select = refalt1_select;
      g_calc_thread_ct = calc_thread_ct;

      const uintptr_t variant_cacheline_ct = QuaterCtToCachelineCt(variant_ct);
      uint32_t output_calc_thread_ct = MINV(calc_thread_ct, variant_cacheline_ct);
      // 4 still seems to be best in AVX2 case
      if (output_calc_thread_ct > 4) {
        output_calc_thread_ct = 4;
      }
      uintptr_t* sample_include;
      uint32_t* sample_include_cumulative_popcounts;
      if (bigstack_alloc_w(raw_sample_ctl, &sample_include) ||
          bigstack_alloc_u32(raw_sample_ctl, &sample_include_cumulative_popcounts) ||
          bigstack_alloc_vp(output_calc_thread_ct, &g_thread_vecaligned_bufs)) {
        goto ExportIndMajorBed_ret_NOMEM;
      }
      for (uint32_t tidx = 0; tidx < output_calc_thread_ct; ++tidx) {
        g_thread_vecaligned_bufs[tidx] = S_CAST(VecW*, bigstack_alloc_raw(kPglQuaterTransposeBufbytes));
      }
      // each of the two write buffers should use <= 1/8 of the remaining
      // workspace
      const uintptr_t writebuf_cachelines_avail = bigstack_left() / (kCacheline * 8);
      uint32_t sample_batch_size = kPglQuaterTransposeBatch;
      if (variant_cacheline_ct * kPglQuaterTransposeBatch > writebuf_cachelines_avail) {
        sample_batch_size = RoundDownPow2(writebuf_cachelines_avail / variant_cacheline_ct, kBitsPerWordD2);
        if (!sample_batch_size) {
          goto ExportIndMajorBed_ret_NOMEM;
        }
      }
      g_smaj_writebufs[0] = S_CAST(uintptr_t*, bigstack_alloc_raw(variant_cacheline_ct * kCacheline * sample_batch_size));
      g_smaj_writebufs[1] = S_CAST(uintptr_t*, bigstack_alloc_raw(variant_cacheline_ct * kCacheline * sample_batch_size));
      const uintptr_t readbuf_vecs_avail = (bigstack_left() / kCacheline) * kVecsPerCacheline;
      if (readbuf_vecs_avail < variant_ct) {
        goto ExportIndMajorBed_ret_NOMEM;
      }
      uintptr_t read_sample_ctv2 = readbuf_vecs_avail / variant_ct;
      uint32_t read_sample_ct;
      if (read_sample_ctv2 >= QuaterCtToVecCt(sample_ct)) {
        read_sample_ct = sample_ct;
      } else {
        read_sample_ct = read_sample_ctv2 * kQuatersPerVec;
      }
      uintptr_t read_sample_ctaw2 = QuaterCtToAlignedWordCt(read_sample_ct);
      uintptr_t* vmaj_readbuf = S_CAST(uintptr_t*, bigstack_alloc_raw_rd(variant_ct * read_sample_ctaw2 * kBytesPerWord));
      g_variant_ct = variant_ct;
      g_output_calc_thread_ct = output_calc_thread_ct;
      g_error_ret = kPglRetSuccess;
      uint32_t sample_uidx_start = AdvTo1Bit(orig_sample_include, 0);
      const uintptr_t variant_ct4 = QuaterCtToByteCt(variant_ct);
      const uintptr_t variant_ctaclw2 = variant_cacheline_ct * kWordsPerCacheline;
      const uint32_t read_block_sizel = BitCtToWordCt(read_block_size);
      const uint32_t read_block_ct_m1 = (raw_variant_ct - 1) / read_block_size;
      const uint32_t pass_ct = 1 + (sample_ct - 1) / read_sample_ct;
      for (uint32_t pass_idx = 0; pass_idx < pass_ct; ++pass_idx) {
        memcpy(sample_include, orig_sample_include, raw_sample_ctl * sizeof(intptr_t));
        if (sample_uidx_start) {
          ClearBitsNz(0, sample_uidx_start, sample_include);
        }
        uint32_t sample_uidx_end;
        if (pass_idx + 1 == pass_ct) {
          read_sample_ct = sample_ct - pass_idx * read_sample_ct;
          read_sample_ctaw2 = QuaterCtToAlignedWordCt(read_sample_ct);
          sample_uidx_end = raw_sample_ct;
        } else {
          sample_uidx_end = FindNth1BitFrom(orig_sample_include, sample_uidx_start + 1, read_sample_ct);
          ClearBitsNz(sample_uidx_end, raw_sample_ct, sample_include);
        }
        FillCumulativePopcounts(sample_include, raw_sample_ctl, sample_include_cumulative_popcounts);
        g_sample_include = sample_include;
        g_sample_include_cumulative_popcounts = sample_include_cumulative_popcounts;
        g_vmaj_readbuf = vmaj_readbuf;
        g_sample_ct = read_sample_ct;
        if (pass_idx) {
          pgfip->block_base = main_loadbufs[0];
          for (uint32_t tidx = 0; tidx < calc_thread_ct; ++tidx) {
            PgrClearLdCache(g_pgr_ptrs[tidx]);
            g_pgr_ptrs[tidx]->fi.block_base = main_loadbufs[0];
            g_pgr_ptrs[tidx]->fi.block_offset = 0;
          }
        }
        uint32_t parity = 0;
        uint32_t read_block_idx = 0;
        uint32_t variant_idx = 0;
        uint32_t is_last_block = 0;
        uint32_t cur_read_block_size = read_block_size;
        uint32_t pct = 0;
        uint32_t next_print_idx = variant_ct / 100;
        putc_unlocked('\r', stdout);
        printf("--export ind-major-bed pass %u/%u: loading... 0%%", pass_idx + 1, pass_ct);
        fflush(stdout);
        while (1) {
          uintptr_t cur_block_write_ct = 0;
          if (!is_last_block) {
            while (read_block_idx < read_block_ct_m1) {
              cur_block_write_ct = PopcountWords(&(variant_include[read_block_idx * read_block_sizel]), read_block_sizel);
              if (cur_block_write_ct) {
                break;
              }
              ++read_block_idx;
            }
            if (read_block_idx == read_block_ct_m1) {
              cur_read_block_size = raw_variant_ct - (read_block_idx * read_block_size);
              cur_block_write_ct = PopcountWords(&(variant_include[read_block_idx * read_block_sizel]), BitCtToWordCt(cur_read_block_size));
            }
            if (PgfiMultiread(variant_include, read_block_idx * read_block_size, read_block_idx * read_block_size + cur_read_block_size, cur_block_write_ct, pgfip)) {
              if (variant_idx) {
                JoinThreads2z(calc_thread_ct, 0, threads);
                g_cur_block_write_ct = 0;
                ErrorCleanupThreads2z(TransposeToSmajReadThread, calc_thread_ct, threads);
              }
              goto ExportIndMajorBed_ret_THREAD_CREATE_FAIL;
            }
          }
          if (variant_idx) {
            JoinThreads2z(calc_thread_ct, is_last_block, threads);
            reterr = g_error_ret;
            if (reterr) {
              if (!is_last_block) {
                g_cur_block_write_ct = 0;
                ErrorCleanupThreads2z(TransposeToSmajReadThread, calc_thread_ct, threads);
              }
              if (reterr == kPglRetMalformedInput) {
                logputs("\n");
                logerrputs("Error: Malformed .pgen file.\n");
              }
              goto ExportIndMajorBed_ret_1;
            }
          }
          if (!is_last_block) {
            g_cur_block_write_ct = cur_block_write_ct;
            ComputeUidxStartPartition(variant_include, cur_block_write_ct, calc_thread_ct, read_block_idx * read_block_size, g_read_variant_uidx_starts);
            for (uint32_t tidx = 0; tidx < calc_thread_ct; ++tidx) {
              g_pgr_ptrs[tidx]->fi.block_base = pgfip->block_base;
              g_pgr_ptrs[tidx]->fi.block_offset = pgfip->block_offset;
            }
            is_last_block = (variant_idx + cur_block_write_ct == variant_ct);
            if (SpawnThreads2z(TransposeToSmajReadThread, calc_thread_ct, is_last_block, threads)) {
              goto ExportIndMajorBed_ret_THREAD_CREATE_FAIL;
            }
          }
          parity = 1 - parity;
          if (variant_idx == variant_ct) {
            break;
          }
          if (variant_idx >= next_print_idx) {
            if (pct > 10) {
              putc_unlocked('\b', stdout);
            }
            pct = (variant_idx * 100LLU) / variant_ct;
            printf("\b\b%u%%", pct++);
            fflush(stdout);
            next_print_idx = (pct * S_CAST(uint64_t, variant_ct)) / 100;
          }

          ++read_block_idx;
          variant_idx += cur_block_write_ct;
          pgfip->block_base = main_loadbufs[parity];
        }
        // 2. Transpose and write.  (Could parallelize some of the transposing
        //    with the read loop, but since we can't write a single row until
        //    the read loop is done, and both write speed and write buffer
        //    space are bottlenecks, that can't be expected to help much.)
        g_sample_batch_size = sample_batch_size;
        parity = 0;
        is_last_block = 0;
        if (pct > 10) {
          fputs("\b \b", stdout);
        }
        fputs("\b\b\b\b\b\b\b\b\b\b\b\b\bwriting... 0%", stdout);
        fflush(stdout);
        pct = 0;
        uint32_t flush_sample_idx = 0;
        uint32_t flush_sample_idx_end = 0;
        next_print_idx = read_sample_ct / 100;
        while (1) {
          if (!is_last_block) {
            is_last_block = (flush_sample_idx_end + sample_batch_size >= read_sample_ct);
            if (is_last_block) {
              g_sample_batch_size = read_sample_ct - flush_sample_idx_end;
            }
            if (SpawnThreads2z(TransposeToPlink1SmajWriteThread, output_calc_thread_ct, is_last_block, threads)) {
              goto ExportIndMajorBed_ret_THREAD_CREATE_FAIL;
            }
          }
          if (flush_sample_idx_end) {
            uintptr_t* smaj_writebuf_iter = g_smaj_writebufs[1 - parity];
            for (; flush_sample_idx < flush_sample_idx_end; ++flush_sample_idx) {
              fwrite_unlocked(smaj_writebuf_iter, variant_ct4, 1, outfile);
              smaj_writebuf_iter = &(smaj_writebuf_iter[variant_ctaclw2]);
            }
            if (flush_sample_idx_end == read_sample_ct) {
              break;
            }
            if (flush_sample_idx_end >= next_print_idx) {
              if (pct > 10) {
                putc_unlocked('\b', stdout);
              }
              pct = (flush_sample_idx_end * 100LLU) / read_sample_ct;
              printf("\b\b%u%%", pct++);
              fflush(stdout);
              next_print_idx = (pct * S_CAST(uint64_t, read_sample_ct)) / 100;
            }
          }
          JoinThreads2z(output_calc_thread_ct, is_last_block, threads);
          if (ferror_unlocked(outfile)) {
            // may as well put this check when there are no threads to clean up
            goto ExportIndMajorBed_ret_WRITE_FAIL;
          }
          parity = 1 - parity;
          flush_sample_idx_end += sample_batch_size;
          if (flush_sample_idx_end > read_sample_ct) {
            flush_sample_idx_end = read_sample_ct;
          }
        }
        if (pct > 10) {
          fputs("\b \b", stdout);
        }
        sample_uidx_start = sample_uidx_end;
      }
      fputs("\b\bdone.\n", stdout);
    }
    if (fclose_null(&outfile)) {
      goto ExportIndMajorBed_ret_WRITE_FAIL;
    }
    logprintfww("--export ind-major-bed: %s written.\n", outname);
  }
  while (0) {
  ExportIndMajorBed_ret_NOMEM:
    reterr = kPglRetNomem;
    break;
  ExportIndMajorBed_ret_OPEN_FAIL:
    reterr = kPglRetOpenFail;
    break;
  ExportIndMajorBed_ret_WRITE_FAIL:
    reterr = kPglRetWriteFail;
    break;
  ExportIndMajorBed_ret_THREAD_CREATE_FAIL:
    reterr = kPglRetThreadCreateFail;
    break;
  }
 ExportIndMajorBed_ret_1:
  fclose_cond(outfile);
  pgfip->block_base = nullptr;
  BigstackReset(bigstack_mark);
  return reterr;
}

static_assert(kDosageMid == 16384, "PrintGenDosage() needs to be updated.");
char* PrintGenDosage(uint32_t rawval, char* start) {
  // Similar to PrintSmallDosage(), but it's complicated by .gen import's
  // quantization step (instead of rounding the numbers directly, they're first
  // converted to bgen-1.1 equivalents).  We check
  //   ((n - 0.75)/16384, (n + 0.75)/16384) for even n
  //   ((n - 0.25)/16384, (n + 0.25)/16384) for odd n
  // due to banker's rounding.
  *start++ = '0' + (rawval / 16384);
  rawval = rawval % 16384;
  if (!rawval) {
    return start;
  }
  *start++ = '.';
  const uint32_t halfwidth_65536ths = 3 - (2 * (rawval % 2));
  // (rawval * 4) is in 65536ths
  // 65536 * 625 = 40960k

  const uint32_t range_top_40960k = (rawval * 4 + halfwidth_65536ths) * 625;
  // this is technically checking a half-open rather than a fully-open
  // interval, but that's fine since we never hit the boundary points
  if ((range_top_40960k % 4096) < 1250 * halfwidth_65536ths) {
    // when this is true, the four-decimal-place approximation is in the range
    // which round-trips back to our original number.
    const uint32_t four_decimal_places = range_top_40960k / 4096;
    return u32toa_trunc4(four_decimal_places, start);
  }

  // we wish to print (100000 * remainder + 8192) / 16384, left-0-padded.  and
  // may as well banker's round too.
  //
  // banker's rounding yields a different result than regular rounding for n/64
  // when n is congruent to 1 mod 4:
  //   1/64 = .015625 -> print 0.01562
  //   3/64 = .046875 -> print 0.04688
  //   5/64 = .078125 -> print 0.07812
  const uint32_t five_decimal_places = ((3125 * rawval + 256) / 512) - ((rawval % 1024) == 256);
  const uint32_t first_decimal_place = five_decimal_places / 10000;
  *start++ = '0' + first_decimal_place;
  const uint32_t last_four_digits = five_decimal_places - first_decimal_place * 10000;
  if (last_four_digits) {
    return u32toa_trunc4(last_four_digits, start);
  }
  return start;
}

BoolErr flexbwrite_flush(char* buf, size_t len, FILE* outfile, BGZF* bgz_outfile) {
  if (outfile) {
    return fwrite_checked(buf, len, outfile);
  }
  return (bgzf_write(bgz_outfile, buf, len) < 0);
}


// these assume buf_flush - buf = kMaxMediumLine
// outfile should be nullptr iff we're doing bgzf compression
BoolErr flexbwrite_flush2(char* buf_flush, FILE* outfile, BGZF* bgz_outfile, char** write_iter_ptr) {
  char* buf = &(buf_flush[-S_CAST(int32_t, kMaxMediumLine)]);
  char* buf_end = *write_iter_ptr;
  *write_iter_ptr = buf;
  return flexbwrite_flush(buf, buf_end - buf, outfile, bgz_outfile);
}

static inline BoolErr flexbwrite_ck(char* buf_flush, FILE* outfile, BGZF* bgz_outfile, char** write_iter_ptr) {
  if ((*write_iter_ptr) < buf_flush) {
    return 0;
  }
  return flexbwrite_flush2(buf_flush, outfile, bgz_outfile, write_iter_ptr);
}

PglErr ExportOxGen(const uintptr_t* sample_include, const uint32_t* sample_include_cumulative_popcounts, const uintptr_t* sex_male, const uintptr_t* variant_include, const ChrInfo* cip, const uint32_t* variant_bps, const char* const* variant_ids, const uintptr_t* variant_allele_idxs, const char* const* allele_storage, const AltAlleleCt* refalt1_select, uint32_t sample_ct, uint32_t variant_ct, uint32_t max_allele_slen, __maybe_unused uint32_t max_thread_ct, ExportfFlags exportf_flags, PgenReader* simple_pgrp, char* outname, char* outname_end, uint32_t* sample_missing_geno_cts) {
  unsigned char* bigstack_mark = g_bigstack_base;
  FILE* outfile = nullptr;
  BGZF* bgz_outfile = nullptr;
  PglErr reterr = kPglRetSuccess;
  {
    const uint32_t sample_ctl2 = QuaterCtToWordCt(sample_ct);
    const uint32_t sample_ctl = BitCtToWordCt(sample_ct);
    uintptr_t* genovec;
    uintptr_t* sex_male_collapsed;
    // if we weren't using bigstack_alloc, this would need to be sample_ctaw2
    if (bigstack_alloc_w(sample_ctl2, &genovec) ||
        bigstack_alloc_w(sample_ctl, &sex_male_collapsed)) {
      goto ExportOxGen_ret_NOMEM;
    }
    CopyBitarrSubset(sex_male, sample_include, sample_ct, sex_male_collapsed);

    // See LoadSampleMissingCtsThread() in plink2_filter.cc.
    // Yes, this is overkill, but the obvious alternative of incrementing
    // sample_missing_geno_cts[] when writing a missing call requires a bit of
    // custom chrY logic anyway.
    const uint32_t acc1_vec_ct = BitCtToVecCt(sample_ct);
    uintptr_t* missing_acc1;
    if (bigstack_calloc_w(acc1_vec_ct * kWordsPerVec * 45, &missing_acc1)) {
      goto ExportOxGen_ret_NOMEM;
    }
    const uint32_t acc4_vec_ct = acc1_vec_ct * 4;
    const uint32_t acc8_vec_ct = acc1_vec_ct * 8;
    uintptr_t* missing_acc4 = &(missing_acc1[acc1_vec_ct * kWordsPerVec]);
    uintptr_t* missing_acc8 = &(missing_acc4[acc4_vec_ct * kWordsPerVec]);
    uintptr_t* missing_acc32 = &(missing_acc8[acc8_vec_ct * kWordsPerVec]);

    uintptr_t* dosage_present = nullptr;
    Dosage* dosage_main = nullptr;
    if (simple_pgrp->fi.gflags & kfPgenGlobalDosagePresent) {
      const uint32_t multiallelic_present = (variant_allele_idxs != nullptr);
      if (bigstack_alloc_dosage(sample_ct * (1 + multiallelic_present), &dosage_main) ||
          bigstack_alloc_w(sample_ctl, &dosage_present)) {
        goto ExportOxGen_ret_NOMEM;
      }
    }
    const uint32_t max_chr_blen = GetMaxChrSlen(cip) + 1;
    // if no dosages, all genotypes are 6 bytes (missing = " 0 0 0")
    // with dosages, we print up to 5 digits past the decimal point, so 7 bytes
    //   + space for each number, 24 bytes max
    const uintptr_t max_geno_slen = 6 + (dosage_present != nullptr) * 18;
    char* chr_buf;  // includes trailing space
    char* writebuf;
    if (bigstack_alloc_c(max_chr_blen, &chr_buf) ||
        bigstack_alloc_c(kMaxMediumLine + max_chr_blen + kMaxIdSlen + 16 + 2 * max_allele_slen + max_geno_slen * sample_ct, &writebuf)) {
      goto ExportOxGen_ret_NOMEM;
    }
    if (!(exportf_flags & kfExportfBgz)) {
      snprintf(outname_end, kMaxOutfnameExtBlen, ".gen");
      if (fopen_checked(outname, FOPEN_WB, &outfile)) {
        goto ExportOxGen_ret_OPEN_FAIL;
      }
    } else {
      snprintf(outname_end, kMaxOutfnameExtBlen, ".gen.gz");
      // may want to move the next few lines of boilerplate into its own
      // function
      bgz_outfile = bgzf_open(outname, "w");
      if (!bgz_outfile) {
        goto ExportOxGen_ret_OPEN_FAIL;
      }
#ifndef _WIN32
      if (max_thread_ct > 1) {
        // subtracting 1 seems best on my 8-16 core Mac/Linux test machines
        // post-libdeflate
        // pretty sure it isn't best on a 2-core machine; should test on 4-core
        // third parameter doesn't actually matter any more, but whatever
        const uint32_t compressor_thread_ct = max_thread_ct - (max_thread_ct > 4);
        if (bgzf_mt(bgz_outfile, MINV(128, compressor_thread_ct), 128)) {
          goto ExportOxGen_ret_NOMEM;
        }
      }
#endif
    }
    char* writebuf_flush = &(writebuf[kMaxMediumLine]);
    char* write_iter = writebuf;
    uint32_t variant_uidx = 0;
    uint32_t chr_blen = 0;

    // although we don't support --set-hh-missing, etc. here, we do still want
    // to be aware of chrY so we can exclude nonmales from the
    // sample_missing_geno_cts update there.
    uint32_t is_y = 0;

    uint32_t chr_fo_idx = UINT32_MAX;
    const uint32_t y_code = cip->xymt_codes[kChrOffsetY];
    uint32_t chr_end = 0;
    uint32_t vidx_rem15 = 15;
    uint32_t vidx_rem255d15 = 17;
    const uint32_t sample_ctl2_m1 = sample_ctl2 - 1;
    const char hardcall_strs[] = " 1 0 0   0 1 0   0 0 1   0 0 0";
    const uint32_t ref_allele_last = !(exportf_flags & kfExportfRefFirst);
    logprintfww5("Writing %s ... ", outname);
    fputs("0%", stdout);
    fflush(stdout);
    uint32_t pct = 0;
    uint32_t next_print_variant_idx = variant_ct / 100;
    uint32_t ref_allele_idx = 0;
    uint32_t alt1_allele_idx = 1;
    for (uint32_t variant_idx = 0; variant_idx < variant_ct; ++variant_idx, ++variant_uidx) {
      MovU32To1Bit(variant_include, &variant_uidx);
      if (variant_uidx >= chr_end) {
        do {
          ++chr_fo_idx;
          chr_end = cip->chr_fo_vidx_start[chr_fo_idx + 1];
        } while (variant_uidx >= chr_end);
        const uint32_t chr_idx = cip->chr_file_order[chr_fo_idx];
        char* chr_name_end = chrtoa(cip, chr_idx, chr_buf);
        // Oxford spec doesn't seem to require spaces for .gen (only .sample),
        // but in practice spaces always seem to be used, and plink 1.9 doesn't
        // let you toggle this, so let's not worry about supporting tabs here
        *chr_name_end++ = ' ';
        chr_blen = chr_name_end - chr_buf;
        is_y = (chr_idx == y_code);
      }
      write_iter = memcpya(write_iter, chr_buf, chr_blen);
      write_iter = strcpyax(write_iter, variant_ids[variant_uidx], ' ');
      write_iter = u32toa_x(variant_bps[variant_uidx], ' ', write_iter);
      uintptr_t variant_allele_idx_base = variant_uidx * 2;
      if (variant_allele_idxs) {
        variant_allele_idx_base = variant_allele_idxs[variant_uidx];
      }
      if (refalt1_select) {
        ref_allele_idx = refalt1_select[variant_uidx * 2];
        alt1_allele_idx = refalt1_select[variant_uidx * 2 + 1];
      }
      // todo: multiallelic case
      uint32_t dosage_ct;
      reterr = PgrGetD(sample_include, sample_include_cumulative_popcounts, sample_ct, variant_uidx, simple_pgrp, genovec, dosage_present, dosage_main, &dosage_ct);
      if (reterr) {
        if (reterr != kPglRetReadFail) {
          logputs("\n");
          logerrputs("Error: Malformed .pgen file.\n");
        }
        goto ExportOxGen_ret_1;
      }
      if (ref_allele_idx + ref_allele_last == 1) {
        assert(!dosage_ct);
        GenovecInvertUnsafe(sample_ct, genovec);
        BiallelicDosage16Invert(dosage_ct, dosage_main);
      }

      const char* const* cur_alleles = &(allele_storage[variant_allele_idx_base]);
      if (ref_allele_last) {
        write_iter = strcpyax(write_iter, cur_alleles[alt1_allele_idx], ' ');
        write_iter = strcpya(write_iter, cur_alleles[ref_allele_idx]);
      } else {
        write_iter = strcpyax(write_iter, cur_alleles[ref_allele_idx], ' ');
        write_iter = strcpya(write_iter, cur_alleles[alt1_allele_idx]);
      }
      uint32_t widx = 0;
      uint32_t inner_loop_last = kBitsPerWordD2 - 1;
      if (!dosage_ct) {
        while (1) {
          if (widx >= sample_ctl2_m1) {
            if (widx > sample_ctl2_m1) {
              break;
            }
            inner_loop_last = (sample_ct - 1) % kBitsPerWordD2;
          }
          uintptr_t geno_word = genovec[widx];
          for (uint32_t sample_idx_lowbits = 0; sample_idx_lowbits <= inner_loop_last; ++sample_idx_lowbits) {
            write_iter = memcpya(write_iter, &(hardcall_strs[(geno_word & 3) * 8]), 6);
            geno_word >>= 2;
          }
          ++widx;
        }
      } else {
        const Halfword* dosage_present_alias = R_CAST(Halfword*, dosage_present);
        const Dosage* dosage_main_iter = dosage_main;
        while (1) {
          if (widx >= sample_ctl2_m1) {
            if (widx > sample_ctl2_m1) {
              break;
            }
            inner_loop_last = (sample_ct - 1) % kBitsPerWordD2;
          }
          uintptr_t geno_word = genovec[widx];
          uint32_t dosage_present_hw = dosage_present_alias[widx];
          if (!dosage_present_hw) {
            for (uint32_t sample_idx_lowbits = 0; sample_idx_lowbits <= inner_loop_last; ++sample_idx_lowbits) {
              write_iter = memcpya(write_iter, &(hardcall_strs[(geno_word & 3) * 8]), 6);
              geno_word >>= 2;
            }
          } else {
            for (uint32_t sample_idx_lowbits = 0; sample_idx_lowbits <= inner_loop_last; ++sample_idx_lowbits) {
              if (dosage_present_hw & 1) {
                const uint32_t dosage_int = *dosage_main_iter++;
                if (dosage_int <= kDosageMid) {
                  *write_iter++ = ' ';
                  write_iter = PrintGenDosage(kDosageMid - dosage_int, write_iter);
                  *write_iter++ = ' ';
                  write_iter = PrintGenDosage(dosage_int, write_iter);
                  write_iter = strcpya(write_iter, " 0");
                } else {
                  assert(dosage_int <= kDosageMax);
                  write_iter = memcpyl3a(write_iter, " 0 ");
                  write_iter = PrintGenDosage(kDosageMax - dosage_int, write_iter);
                  *write_iter++ = ' ';
                  write_iter = PrintGenDosage(dosage_int - kDosageMid, write_iter);
                }
              } else {
                write_iter = memcpya(write_iter, &(hardcall_strs[(geno_word & 3) * 8]), 6);
              }
              geno_word >>= 2;
              dosage_present_hw >>= 1;
            }
          }
          ++widx;
        }
      }
      AppendBinaryEoln(&write_iter);
      if (flexbwrite_ck(writebuf_flush, outfile, bgz_outfile, &write_iter)) {
        goto ExportOxGen_ret_WRITE_FAIL;
      }
      // bugfix (13 Apr 2018): this missingness calculation was only taking
      // hardcalls into account, which is inappropriate for .gen/.bgen
      GenovecToMissingnessUnsafe(genovec, sample_ct, missing_acc1);
      if (dosage_ct) {
        BitvecAndNot(dosage_present, sample_ctl, missing_acc1);
      }
      if (is_y) {
        BitvecAnd(sex_male_collapsed, sample_ctl, missing_acc1);
      }
      VcountIncr1To4(missing_acc1, acc1_vec_ct, missing_acc4);
      if (!(--vidx_rem15)) {
        Vcount0Incr4To8(acc4_vec_ct, missing_acc4, missing_acc8);
        vidx_rem15 = 15;
        if (!(--vidx_rem255d15)) {
          Vcount0Incr8To32(acc8_vec_ct, missing_acc8, missing_acc32);
          vidx_rem255d15 = 17;
        }
      }
      if (variant_idx >= next_print_variant_idx) {
        if (pct > 10) {
          putc_unlocked('\b', stdout);
        }
        pct = (variant_idx * 100LLU) / variant_ct;
        printf("\b\b%u%%", pct++);
        fflush(stdout);
        next_print_variant_idx = (pct * S_CAST(uint64_t, variant_ct)) / 100;
      }
    }
    if (write_iter != writebuf) {
      // this should be wrapped...
      if (flexbwrite_flush(writebuf, write_iter - writebuf, outfile, bgz_outfile)) {
        goto ExportOxGen_ret_WRITE_FAIL;
      }
    }
    if (bgz_outfile) {
      if (bgzf_close(bgz_outfile)) {
        bgz_outfile = nullptr;
        goto ExportOxGen_ret_WRITE_FAIL;
      }
      bgz_outfile = nullptr;
    } else {
      if (fclose_null(&outfile)) {
        goto ExportOxGen_ret_WRITE_FAIL;
      }
    }
    if (pct > 10) {
      putc_unlocked('\b', stdout);
    }
    fputs("\b\b", stdout);
    logprintf("done.\n");
    VcountIncr4To8(missing_acc4, acc4_vec_ct, missing_acc8);
    VcountIncr8To32(missing_acc8, acc8_vec_ct, missing_acc32);
    uint32_t* scrambled_missing_cts = R_CAST(uint32_t*, missing_acc32);
    for (uint32_t sample_idx = 0; sample_idx < sample_ct; ++sample_idx) {
      const uint32_t scrambled_idx = VcountScramble1(sample_idx);
      sample_missing_geno_cts[sample_idx] = scrambled_missing_cts[scrambled_idx];
    }
  }
  while (0) {
  ExportOxGen_ret_NOMEM:
    reterr = kPglRetNomem;
    break;
  ExportOxGen_ret_OPEN_FAIL:
    reterr = kPglRetOpenFail;
    break;
  ExportOxGen_ret_WRITE_FAIL:
    reterr = kPglRetWriteFail;
    break;
  }
 ExportOxGen_ret_1:
  fclose_cond(outfile);
  if (bgz_outfile) {
    bgzf_close(bgz_outfile);
  }
  BigstackReset(bigstack_mark);
  return reterr;
}

#ifdef __arm__
#  error "Unaligned accesses in ExportOxHapslegend()."
#endif
PglErr ExportOxHapslegend(const uintptr_t* sample_include, const uint32_t* sample_include_cumulative_popcounts, const uintptr_t* sex_male_collapsed, const uintptr_t* variant_include, const ChrInfo* cip, const uint32_t* variant_bps, const char* const* variant_ids, const uintptr_t* variant_allele_idxs, const char* const* allele_storage, const AltAlleleCt* refalt1_select, uint32_t sample_ct, uint32_t raw_variant_ct, uint32_t variant_ct, uint32_t max_allele_slen, ExportfFlags exportf_flags, PgenReader* simple_pgrp, char* outname, char* outname_end) {
  assert(sample_ct);
  assert(variant_ct);
  unsigned char* bigstack_mark = g_bigstack_base;
  FILE* outfile = nullptr;
  PglErr reterr = kPglRetSuccess;
  {
    const uint32_t sample_ctl = BitCtToWordCt(sample_ct);
    const uint32_t just_haps = (exportf_flags / kfExportfHaps) & 1;
    const uint32_t male_ct = PopcountWords(sex_male_collapsed, sample_ctl);
    if (XymtIsNonempty(variant_include, cip, kChrOffsetY) && (male_ct != sample_ct)) {
      logerrprintf("Error: '--export haps%s' must exclude chrY unless the dataset is all-male.\n", just_haps? "" : "legend");
      goto ExportOxHapslegend_ret_INCONSISTENT_INPUT;
    }
    const uint32_t ref_allele_last = !(exportf_flags & kfExportfRefFirst);
    const uint32_t x_code = cip->xymt_codes[kChrOffsetX];
    char* chr_buf = nullptr;
    uint32_t is_x = 0;
    uint32_t is_haploid = 0;
    uint32_t variant_uidx = AdvTo1Bit(variant_include, 0);
    uint32_t chr_fo_idx = UINT32_MAX;
    uint32_t chr_end = 0;
    uint32_t ref_allele_idx = 0;
    uint32_t alt1_allele_idx = 1;
    uintptr_t writebuf_alloc = 0;
    if (!just_haps) {
      // .legend doesn't have a chromosome column, so verify we only need to
      // export a single chromosome
      const uint32_t variant_uidx_start = variant_uidx;
      chr_fo_idx = GetVariantChrFoIdx(cip, variant_uidx_start);
      chr_end = cip->chr_fo_vidx_start[chr_fo_idx + 1];
      if ((chr_end != raw_variant_ct) && (PopcountBitRange(variant_include, variant_uidx_start, chr_end) != variant_ct)) {
        logerrputs("Error: '--export hapslegend' does not support multiple chromosomes.\n");
        goto ExportOxHapslegend_ret_INCONSISTENT_INPUT;
      }
      const uint32_t chr_idx = cip->chr_file_order[chr_fo_idx];
      is_x = (chr_idx == x_code);
      is_haploid = IsSet(cip->haploid_mask, chr_idx);
      snprintf(outname_end, kMaxOutfnameExtBlen, ".legend");
      if (fopen_checked(outname, FOPEN_WB, &outfile)) {
        goto ExportOxHapslegend_ret_OPEN_FAIL;
      }
      char* writebuf;
      if (bigstack_alloc_c(kMaxMediumLine + kMaxIdSlen + 32 + 2 * max_allele_slen, &writebuf)) {
        goto ExportOxHapslegend_ret_NOMEM;
      }
      char* writebuf_flush = &(writebuf[kMaxMediumLine]);
      char* write_iter = strcpya(writebuf, "id position a0 a1" EOLN_STR);
      logprintfww5("Writing %s ... ", outname);
      fflush(stdout);
      for (uint32_t variant_idx = 0; variant_idx < variant_ct; ++variant_idx, ++variant_uidx) {
        MovU32To1Bit(variant_include, &variant_uidx);
        write_iter = strcpyax(write_iter, variant_ids[variant_uidx], ' ');
        write_iter = u32toa_x(variant_bps[variant_uidx], ' ', write_iter);
        if (refalt1_select) {
          ref_allele_idx = refalt1_select[variant_uidx * 2];
          alt1_allele_idx = refalt1_select[variant_uidx * 2 + 1];
        }
        uintptr_t variant_allele_idx_base = variant_uidx * 2;
        if (variant_allele_idxs) {
          variant_allele_idx_base = variant_allele_idxs[variant_uidx];
        }
        const char* const* cur_alleles = &(allele_storage[variant_allele_idx_base]);
        if (ref_allele_last) {
          write_iter = strcpyax(write_iter, cur_alleles[alt1_allele_idx], ' ');
          write_iter = strcpya(write_iter, cur_alleles[ref_allele_idx]);
        } else {
          write_iter = strcpyax(write_iter, cur_alleles[ref_allele_idx], ' ');
          write_iter = strcpya(write_iter, cur_alleles[alt1_allele_idx]);
        }
        AppendBinaryEoln(&write_iter);
        if (fwrite_ck(writebuf_flush, outfile, &write_iter)) {
          goto ExportOxHapslegend_ret_WRITE_FAIL;
        }
      }
      if (fclose_flush_null(writebuf_flush, write_iter, &outfile)) {
        goto ExportOxHapslegend_ret_WRITE_FAIL;
      }
      logputs("done.\n");
      variant_uidx = variant_uidx_start;
      BigstackReset(writebuf);
    } else {
      const uint32_t max_chr_blen = GetMaxChrSlen(cip) + 1;
      if (bigstack_alloc_c(max_chr_blen, &chr_buf)) {
        goto ExportOxHapslegend_ret_NOMEM;
      }
      writebuf_alloc = max_chr_blen + kMaxIdSlen + 32 + 2 * max_allele_slen;
    }
    writebuf_alloc += kMaxMediumLine + (4 * k1LU) * sample_ct + kCacheline;
    const uint32_t sample_ctv = BitCtToVecCt(sample_ct);
    const uint32_t sample_ctl2 = QuaterCtToWordCt(sample_ct);
    const uint32_t sample_ctl2_m1 = sample_ctl2 - 1;
    char* writebuf;
    uintptr_t* sex_male_collapsed_interleaved;
    uintptr_t* genovec;
    uintptr_t* phasepresent;
    uintptr_t* phaseinfo;
    if (bigstack_alloc_w(sample_ctv * kWordsPerVec, &sex_male_collapsed_interleaved) ||
        bigstack_alloc_c(writebuf_alloc, &writebuf) ||
        bigstack_alloc_w(sample_ctl2, &genovec) ||
        bigstack_alloc_w(sample_ctl, &phasepresent) ||
        bigstack_alloc_w(sample_ctl, &phaseinfo)) {
      goto ExportOxHapslegend_ret_NOMEM;
    }
    // sex_male_collapsed had trailing bits zeroed out
    FillInterleavedMaskVec(sex_male_collapsed, sample_ctv, sex_male_collapsed_interleaved);
    // assumes little-endian
    // 3 = 1|0, not missing
    // 4..7 = male chrX
    // user's responsibility to split off PARs
    uint32_t genotext[7];
    genotext[0] = 0x20302030;
    genotext[2] = 0x20312031;
    genotext[4] = 0x202d2030;
    genotext[6] = 0x202d2031;
    if (ref_allele_last) {
      genotext[1] = 0x20302031;
      genotext[3] = 0x20312030;
    } else {
      genotext[1] = 0x20312030;
      genotext[3] = 0x20302031;
    }
#ifndef NDEBUG
    genotext[5] = 0x21475542;  // "BUG!"
#endif
    uint32_t* cur_genotext = genotext;
    if (is_haploid && (!is_x)) {
      cur_genotext = &(genotext[4]);
    }
    char* writebuf_flush = &(writebuf[kMaxMediumLine]);
    char* write_iter = writebuf;
    snprintf(outname_end, kMaxOutfnameExtBlen, ".haps");
    if (fopen_checked(outname, FOPEN_WB, &outfile)) {
      goto ExportOxHapslegend_ret_OPEN_FAIL;
    }
    logprintfww5("Writing %s ... ", outname);
    fputs("0%", stdout);
    fflush(stdout);
    uint32_t chr_blen = 0;
    uint32_t pct = 0;
    uint32_t next_print_variant_idx = variant_ct / 100;
    for (uint32_t variant_idx = 0; variant_idx < variant_ct; ++variant_idx, ++variant_uidx) {
      MovU32To1Bit(variant_include, &variant_uidx);
      if (variant_uidx >= chr_end) {
        do {
          ++chr_fo_idx;
          chr_end = cip->chr_fo_vidx_start[chr_fo_idx + 1];
        } while (variant_uidx >= chr_end);
        const uint32_t chr_idx = cip->chr_file_order[chr_fo_idx];
        char* chr_name_end = chrtoa(cip, chr_idx, chr_buf);
        *chr_name_end++ = ' ';
        chr_blen = chr_name_end - chr_buf;
        is_x = (chr_idx == x_code);
        is_haploid = IsSet(cip->haploid_mask, chr_idx);
        if ((!is_haploid) || is_x) {
          cur_genotext = genotext;
        } else {
          cur_genotext = &(genotext[4]);
        }
      }
      uint32_t phasepresent_ct;
      reterr = PgrGetP(sample_include, sample_include_cumulative_popcounts, sample_ct, variant_uidx, simple_pgrp, genovec, phasepresent, phaseinfo, &phasepresent_ct);
      if (reterr) {
        goto ExportOxHapslegend_ret_PGR_FAIL;
      }
      ZeroTrailingQuaters(sample_ct, genovec);
      if (!phasepresent_ct) {
        // phaseinfo is NOT cleared in this case
        ZeroWArr(sample_ctl, phaseinfo);
      }
      uint32_t genocounts[4];
      GenovecCountFreqsUnsafe(genovec, sample_ct, genocounts);
      if (phasepresent_ct != genocounts[1]) {
        logputs("\n");
        logerrprintf("Error: '--export haps%s' must be used with a fully phased dataset.\n", just_haps? "" : "legend");
        goto ExportOxHapslegend_ret_INCONSISTENT_INPUT;
      } else if (genocounts[3]) {
        logputs("\n");
        logerrprintf("Error: '--export haps%s' cannot be used with missing genotype calls.\n", just_haps? "" : "legend");
        goto ExportOxHapslegend_ret_INCONSISTENT_INPUT;
      }
      if (is_haploid) {
        // verify that there are no het haploids/mixed MTs
        if (is_x) {
          GenovecCountSubsetFreqs(genovec, sex_male_collapsed_interleaved, sample_ct, male_ct, genocounts);
        }
        if (genocounts[1]) {
          logputs("\n");
          logerrprintfww("Error: '--export haps%s' cannot be used when heterozygous haploid or mixed MT calls are present.%s\n", just_haps? "" : "legend", (is_x && (variant_bps[variant_uidx] <= 2781479))? " (Did you forget --split-par?)" : "");
          goto ExportOxHapslegend_ret_INCONSISTENT_INPUT;
        }
      }
      uintptr_t variant_allele_idx_base = variant_uidx * 2;
      if (variant_allele_idxs) {
        variant_allele_idx_base = variant_allele_idxs[variant_uidx];
      }
      if (refalt1_select) {
        ref_allele_idx = refalt1_select[variant_uidx * 2];
        alt1_allele_idx = refalt1_select[variant_uidx * 2 + 1];
      }
      // this logic only works in the biallelic case
      if (ref_allele_last + ref_allele_idx == 1) {
        GenovecInvertUnsafe(sample_ct, genovec);
        ZeroTrailingQuaters(sample_ct, genovec);
        // bugfix (31 Mar 2018): need to update phaseinfo
        if (phasepresent_ct) {
          // trailing bits don't matter
          BitvecInvert(sample_ctl, phaseinfo);
        }
      }
      if (just_haps) {
        write_iter = memcpya(write_iter, chr_buf, chr_blen);
        write_iter = strcpyax(write_iter, variant_ids[variant_uidx], ' ');
        write_iter = u32toa_x(variant_bps[variant_uidx], ' ', write_iter);
        const char* const* cur_alleles = &(allele_storage[variant_allele_idx_base]);
        if (ref_allele_last) {
          write_iter = strcpyax(write_iter, cur_alleles[alt1_allele_idx], ' ');
          write_iter = strcpya(write_iter, cur_alleles[ref_allele_idx]);
        } else {
          write_iter = strcpyax(write_iter, cur_alleles[ref_allele_idx], ' ');
          write_iter = strcpya(write_iter, cur_alleles[alt1_allele_idx]);
        }
        *write_iter++ = ' ';
      }
      uint32_t* write_iter_ui_alias = R_CAST(uint32_t*, write_iter);
      uint32_t inner_loop_last = kBitsPerWordD2 - 1;
      uint32_t widx = 0;
      if (!is_x) {
        while (1) {
          if (widx >= sample_ctl2_m1) {
            if (widx > sample_ctl2_m1) {
              break;
            }
            inner_loop_last = (sample_ct - 1) % kBitsPerWordD2;
          }
          uintptr_t genovec_word = genovec[widx];
          const uint32_t phaseinfo_halfword = R_CAST(Halfword*, phaseinfo)[widx];
          for (uint32_t sample_idx_lowbits = 0; sample_idx_lowbits <= inner_loop_last; ++sample_idx_lowbits) {
            const uintptr_t cur_geno = genovec_word & 3;
            *write_iter_ui_alias++ = cur_genotext[cur_geno + 2 * ((phaseinfo_halfword >> sample_idx_lowbits) & 1)];
            genovec_word >>= 2;
          }
          ++widx;
        }
      } else {
        while (1) {
          if (widx >= sample_ctl2_m1) {
            if (widx > sample_ctl2_m1) {
              break;
            }
            inner_loop_last = (sample_ct - 1) % kBitsPerWordD2;
          }
          // can operate on genovec halfword + phaseinfo/male quarterwords
          // instead, and pre-shuffle?  test this.
          uintptr_t genovec_word = genovec[widx];
          const uint32_t phaseinfo_halfword = R_CAST(Halfword*, phaseinfo)[widx];
          const uint32_t male_halfword = R_CAST(const Halfword*, sex_male_collapsed)[widx];

          for (uint32_t sample_idx_lowbits = 0; sample_idx_lowbits <= inner_loop_last; ++sample_idx_lowbits) {
            const uintptr_t cur_geno = genovec_word & 3;
            if (cur_geno == 2) {
              assert(!((phaseinfo_halfword >> sample_idx_lowbits) & 1));
            }
            *write_iter_ui_alias++ = cur_genotext[cur_geno + 2 * ((phaseinfo_halfword >> sample_idx_lowbits) & 1) + 4 * ((male_halfword >> sample_idx_lowbits) & 1)];
            genovec_word >>= 2;
          }
          ++widx;
        }
      }
      write_iter = R_CAST(char*, write_iter_ui_alias);
      DecrAppendBinaryEoln(&write_iter);
      if (fwrite_ck(writebuf_flush, outfile, &write_iter)) {
        goto ExportOxHapslegend_ret_WRITE_FAIL;
      }
      if (variant_idx >= next_print_variant_idx) {
        if (pct > 10) {
          putc_unlocked('\b', stdout);
        }
        pct = (variant_idx * 100LLU) / variant_ct;
        printf("\b\b%u%%", pct++);
        fflush(stdout);
        next_print_variant_idx = (pct * S_CAST(uint64_t, variant_ct)) / 100;
      }
    }
    if (fclose_flush_null(writebuf_flush, write_iter, &outfile)) {
      goto ExportOxHapslegend_ret_WRITE_FAIL;
    }
    if (pct > 10) {
      putc_unlocked('\b', stdout);
    }
    fputs("\b\b", stdout);
    logprintf("done.\n");
  }
  while (0) {
  ExportOxHapslegend_ret_NOMEM:
    reterr = kPglRetNomem;
    break;
  ExportOxHapslegend_ret_OPEN_FAIL:
    reterr = kPglRetOpenFail;
    break;
  ExportOxHapslegend_ret_WRITE_FAIL:
    reterr = kPglRetWriteFail;
    break;
  ExportOxHapslegend_ret_INCONSISTENT_INPUT:
    reterr = kPglRetInconsistentInput;
    break;
  ExportOxHapslegend_ret_PGR_FAIL:
    if (reterr != kPglRetReadFail) {
      logputs("\n");
      logerrputs("Error: Malformed .pgen file.\n");
    }
  }
  fclose_cond(outfile);
  BigstackReset(bigstack_mark);
  return reterr;
}

// more multithread globals
static uint16_t** g_bgen_geno_bufs = nullptr;  // per-thread
static struct libdeflate_compressor** g_libdeflate_compressors = nullptr;

static uintptr_t* g_sex_male_collapsed = nullptr;
static uintptr_t** g_missing_acc1 = nullptr;
static uint32_t* g_variant_bytects[2] = {nullptr, nullptr};
static uint32_t g_ref_allele_last = 0;
static uint32_t g_bgen_compressed_buf_max = 0;
static uint32_t g_y_start = 0;
static uint32_t g_y_end = 0;

static const uint16_t bgen11_hardcall_usis[] = {32768, 0, 0, 0,
                                                0, 32768, 0, 0,
                                                0, 0, 32768, 0,
                                                0, 0, 0, 0};

THREAD_FUNC_DECL ExportBgen11Thread(void* arg) {
  const uintptr_t tidx = R_CAST(uintptr_t, arg);
  PgenReader* pgrp = g_pgr_ptrs[tidx];
  uintptr_t* genovec = g_genovecs[tidx];
  const uint32_t sample_ct = g_sample_ct;
  const uint32_t acc1_vec_ct = BitCtToVecCt(sample_ct);
  const uint32_t acc4_vec_ct = acc1_vec_ct * 4;
  const uint32_t acc8_vec_ct = acc1_vec_ct * 8;
  uintptr_t* missing_acc1 = g_missing_acc1[tidx];
  uintptr_t* missing_acc4 = &(missing_acc1[acc1_vec_ct * kWordsPerVec]);
  uintptr_t* missing_acc8 = &(missing_acc4[acc4_vec_ct * kWordsPerVec]);
  uintptr_t* missing_acc32 = &(missing_acc8[acc8_vec_ct * kWordsPerVec]);
  uintptr_t* dosage_present = g_dosage_presents? g_dosage_presents[tidx] : nullptr;
  Dosage* dosage_main = dosage_present? g_dosage_mains[tidx] : nullptr;
  uint16_t* bgen_geno_buf = g_bgen_geno_bufs[tidx];
  struct libdeflate_compressor* compressor = g_libdeflate_compressors[tidx];
  const uintptr_t* variant_include = g_variant_include;
  const uintptr_t* sample_include = g_sample_include;
  const uint32_t* sample_include_cumulative_popcounts = g_sample_include_cumulative_popcounts;
  const uintptr_t* sex_male_collapsed = g_sex_male_collapsed;
  const uint32_t calc_thread_ct = g_calc_thread_ct;
  const uint32_t sample_ctl2_m1 = QuaterCtToWordCt(sample_ct) - 1;
  const uint32_t sample_ctl = BitCtToWordCt(sample_ct);
  const uint32_t bgen_geno_buf_blen = 6 * sample_ct;
  const uint32_t bgen_compressed_buf_max = g_bgen_compressed_buf_max;
  const AltAlleleCt* refalt1_select = g_refalt1_select;
  uint32_t is_y = 0;
  uint32_t y_thresh = g_y_start;
  const uint32_t y_end = g_y_end;
  const uint32_t ref_allele_last = g_ref_allele_last;
  uint32_t vidx_rem15 = 15;
  uint32_t vidx_rem255d15 = 17;
  uint32_t ref_allele_idx = 0;
  uint32_t parity = 0;
  ZeroWArr(acc1_vec_ct * kWordsPerVec * 45, missing_acc1);
  while (1) {
    const uint32_t is_last_block = g_is_last_thread_block;
    const uintptr_t cur_block_write_ct = g_cur_block_write_ct;
    uint32_t write_idx = (tidx * cur_block_write_ct) / calc_thread_ct;
    const uint32_t write_idx_end = ((tidx + 1) * cur_block_write_ct) / calc_thread_ct;
    unsigned char* writebuf_iter = &(g_writebufs[parity][write_idx * S_CAST(uintptr_t, bgen_compressed_buf_max)]);
    uint32_t* variant_bytect_iter = &(g_variant_bytects[parity][write_idx]);
    uint32_t variant_uidx = g_read_variant_uidx_starts[tidx];
    for (; write_idx < write_idx_end; ++write_idx, ++variant_uidx) {
      MovU32To1Bit(variant_include, &variant_uidx);
      if (variant_uidx >= y_thresh) {
        if (variant_uidx < y_end) {
          y_thresh = y_end;
          is_y = 1;
        } else {
          y_thresh = UINT32_MAX;
          is_y = 0;
        }
      }
      if (refalt1_select) {
        ref_allele_idx = refalt1_select[variant_uidx * 2];
      }
      uint32_t dosage_ct;
      PglErr reterr = PgrGetD(sample_include, sample_include_cumulative_popcounts, sample_ct, variant_uidx, pgrp, genovec, dosage_present, dosage_main, &dosage_ct);
      if (reterr) {
        g_error_ret = reterr;
        break;
      }
      if (ref_allele_idx + ref_allele_last == 1) {
        GenovecInvertUnsafe(sample_ct, genovec);
        BiallelicDosage16Invert(dosage_ct, dosage_main);
      }
      uint32_t widx = 0;
      uint32_t inner_loop_last = kBitsPerWordD2 - 1;
      uint16_t* bgen_geno_buf_iter = bgen_geno_buf;
      if (!dosage_ct) {
        while (1) {
          if (widx >= sample_ctl2_m1) {
            if (widx > sample_ctl2_m1) {
              break;
            }
            inner_loop_last = (sample_ct - 1) % kBitsPerWordD2;
          }
          uintptr_t geno_word = genovec[widx];
          for (uint32_t sample_idx_lowbits = 0; sample_idx_lowbits <= inner_loop_last; ++sample_idx_lowbits) {
            memcpy(bgen_geno_buf_iter, &(bgen11_hardcall_usis[(geno_word & 3) * 4]), 6);
            bgen_geno_buf_iter = &(bgen_geno_buf_iter[3]);
            geno_word >>= 2;
          }
          ++widx;
        }
      } else {
        const Halfword* dosage_present_alias = R_CAST(Halfword*, dosage_present);
        const Dosage* dosage_main_iter = dosage_main;
        while (1) {
          if (widx >= sample_ctl2_m1) {
            if (widx > sample_ctl2_m1) {
              break;
            }
            inner_loop_last = (sample_ct - 1) % kBitsPerWordD2;
          }
          uintptr_t geno_word = genovec[widx];
          uint32_t dosage_present_hw = dosage_present_alias[widx];
          if (!dosage_present_hw) {
            for (uint32_t sample_idx_lowbits = 0; sample_idx_lowbits <= inner_loop_last; ++sample_idx_lowbits) {
              memcpy(bgen_geno_buf_iter, &(bgen11_hardcall_usis[(geno_word & 3) * 4]), 6);
              bgen_geno_buf_iter = &(bgen_geno_buf_iter[3]);
              geno_word >>= 2;
            }
          } else {
            for (uint32_t sample_idx_lowbits = 0; sample_idx_lowbits <= inner_loop_last; ++sample_idx_lowbits) {
              if (dosage_present_hw & 1) {
                uint32_t dosage_int = *dosage_main_iter++;
                dosage_int *= 2;
                if (dosage_int <= kDosageMax) {
                  *bgen_geno_buf_iter++ = kDosageMax - dosage_int;
                  *bgen_geno_buf_iter++ = dosage_int;
                  *bgen_geno_buf_iter++ = 0;
                } else {
                  dosage_int -= kDosageMax;
                  *bgen_geno_buf_iter++ = 0;
                  *bgen_geno_buf_iter++ = kDosageMax - dosage_int;
                  *bgen_geno_buf_iter++ = dosage_int;
                }
              } else {
                memcpy(bgen_geno_buf_iter, &(bgen11_hardcall_usis[(geno_word & 3) * 4]), 6);
                bgen_geno_buf_iter = &(bgen_geno_buf_iter[3]);
              }
              geno_word >>= 2;
              dosage_present_hw >>= 1;
            }
          }
          ++widx;
        }
      }
      uintptr_t compressed_blen = libdeflate_zlib_compress(compressor, bgen_geno_buf, bgen_geno_buf_blen, writebuf_iter, bgen_compressed_buf_max);
      if (!compressed_blen) {
        // is this actually possible?
        g_error_ret = kPglRetNomem;
        break;
      }
      *variant_bytect_iter++ = compressed_blen;
      writebuf_iter = &(writebuf_iter[bgen_compressed_buf_max]);
      // bugfix (13 Apr 2018): this missingness calculation was only taking
      // hardcalls into account, which is inappropriate for .gen/.bgen
      GenovecToMissingnessUnsafe(genovec, sample_ct, missing_acc1);
      if (dosage_ct) {
        BitvecAndNot(dosage_present, sample_ctl, missing_acc1);
      }
      if (is_y) {
        BitvecAnd(sex_male_collapsed, sample_ctl, missing_acc1);
      }
      VcountIncr1To4(missing_acc1, acc1_vec_ct, missing_acc4);
      if (!(--vidx_rem15)) {
        Vcount0Incr4To8(acc4_vec_ct, missing_acc4, missing_acc8);
        vidx_rem15 = 15;
        if (!(--vidx_rem255d15)) {
          Vcount0Incr8To32(acc8_vec_ct, missing_acc8, missing_acc32);
          vidx_rem255d15 = 17;
        }
      }
    }
    if (is_last_block) {
      VcountIncr4To8(missing_acc4, acc4_vec_ct, missing_acc8);
      VcountIncr8To32(missing_acc8, acc8_vec_ct, missing_acc32);
      THREAD_RETURN;
    }
    THREAD_BLOCK_FINISH(tidx);
    parity = 1 - parity;
  }
}

PglErr ExportBgen11(const char* outname, const uintptr_t* sample_include, uint32_t* sample_include_cumulative_popcounts, const uintptr_t* sex_male, const uintptr_t* variant_include, const ChrInfo* cip, const uint32_t* variant_bps, const char* const* variant_ids, const uintptr_t* variant_allele_idxs, const char* const* allele_storage, const AltAlleleCt* refalt1_select, uint32_t sample_ct, uint32_t raw_variant_ct, uint32_t variant_ct, uint32_t max_allele_slen, uint32_t max_thread_ct, ExportfFlags exportf_flags, uintptr_t pgr_alloc_cacheline_ct, PgenFileInfo* pgfip, uint32_t* sample_missing_geno_cts) {
  // isomorphic to ExportOxGen().
  assert(sample_ct);
  unsigned char* bigstack_mark = g_bigstack_base;
  FILE* outfile = nullptr;
  PglErr reterr = kPglRetSuccess;
  {
    const uint32_t max_chr_slen = GetMaxChrSlen(cip);
    g_libdeflate_compressors = S_CAST(struct libdeflate_compressor**, bigstack_alloc(max_thread_ct * sizeof(intptr_t)));
    if (!g_libdeflate_compressors) {
      goto ExportBgen11_ret_NOMEM;
    }
    ZeroPtrArr(max_thread_ct, g_libdeflate_compressors);
    // allocate the first compressor so we can call
    // libdeflate_zlib_compress_bound().
    // could add a --bgz-level flag analogous to --zst-level, of course.
    g_libdeflate_compressors[0] = libdeflate_alloc_compressor(6);
    if (!g_libdeflate_compressors[0]) {
      goto ExportBgen11_ret_NOMEM;
    }
    const uintptr_t bgen_compressed_buf_max = libdeflate_zlib_compress_bound(g_libdeflate_compressors[0], 6LU * sample_ct);
#ifdef __LP64__
    if (bgen_compressed_buf_max > UINT32_MAX) {
      logerrputs("Error: Too many samples for .bgen format.\n");
      goto ExportBgen11_ret_INCONSISTENT_INPUT;
    }
#endif
    g_bgen_compressed_buf_max = bgen_compressed_buf_max;
    const uint32_t sample_ctl = BitCtToWordCt(sample_ct);
    const uintptr_t writebuf_len = bgen_compressed_buf_max + 2 * max_allele_slen + 2 * kMaxIdSlen + 32;
    char* chr_buf;
    unsigned char* writebuf;
    if (bigstack_alloc_c(max_chr_slen, &chr_buf) ||
        bigstack_alloc_uc(writebuf_len, &writebuf) ||
        bigstack_alloc_w(sample_ctl, &g_sex_male_collapsed)) {
      goto ExportBgen11_ret_NOMEM;
    }
    CopyBitarrSubset(sex_male, sample_include, sample_ct, g_sex_male_collapsed);

    const uintptr_t max_write_block_byte_ct = bigstack_left() / 4;
    uint32_t max_write_block_size = kPglVblockSize;
    while (1) {
      // limit each write buffer to 1/4 of remaining workspace
      if (S_CAST(uint64_t, bgen_compressed_buf_max + sizeof(int32_t)) * max_write_block_size <= max_write_block_byte_ct) {
        break;
      }
      if (max_write_block_size <= kBitsPerVec) {
        goto ExportBgen11_ret_NOMEM;
      }
      max_write_block_size /= 2;
    }
    uint32_t calc_thread_ct = (max_thread_ct > 2)? (max_thread_ct - 1) : max_thread_ct;
    // seems to saturate around this point
    // todo: retest with libdeflate
    if (calc_thread_ct > 15) {
      calc_thread_ct = 15;
    }
    if (bigstack_alloc_uc(bgen_compressed_buf_max * max_write_block_size, &(g_writebufs[0])) ||
        bigstack_alloc_uc(bgen_compressed_buf_max * max_write_block_size, &(g_writebufs[1])) ||
        bigstack_alloc_u32(max_write_block_size, &(g_variant_bytects[0])) ||
        bigstack_alloc_u32(max_write_block_size, &(g_variant_bytects[1])) ||
        bigstack_alloc_wp(calc_thread_ct, &g_missing_acc1) ||
        bigstack_alloc_u16p(calc_thread_ct, &g_bgen_geno_bufs)) {
      goto ExportBgen11_ret_NOMEM;
    }
    // we allocated [0] earlier
    for (uint32_t tidx = 1; tidx < calc_thread_ct; ++tidx) {
      g_libdeflate_compressors[tidx] = libdeflate_alloc_compressor(6);
      if (!g_libdeflate_compressors[tidx]) {
        goto ExportBgen11_ret_NOMEM;
      }
    }

    const uint32_t acc1_vec_ct = BitCtToVecCt(sample_ct);
    const uint32_t dosage_is_present = pgfip->gflags & kfPgenGlobalDosagePresent;
    const uintptr_t track_missing_cacheline_ct = VecCtToCachelineCt(acc1_vec_ct * 45);
    // no overflow risk here, thanks to compress_bound() check above
    const uintptr_t bgen_geno_cacheline_ct = DivUp(6 * sample_ct, kCacheline);
    const uintptr_t thread_xalloc_cacheline_ct = track_missing_cacheline_ct + bgen_geno_cacheline_ct;
    unsigned char* main_loadbufs[2];
    pthread_t* threads;
    uint32_t read_block_size;
    if (PgenMtLoadInit(variant_include, sample_ct, variant_ct, bigstack_left(), pgr_alloc_cacheline_ct, thread_xalloc_cacheline_ct, 0, pgfip, &calc_thread_ct, &g_genovecs, nullptr, nullptr, dosage_is_present? (&g_dosage_presents) : nullptr, dosage_is_present? (&g_dosage_mains) : nullptr, nullptr, nullptr, &read_block_size, main_loadbufs, &threads, &g_pgr_ptrs, &g_read_variant_uidx_starts)) {
      goto ExportBgen11_ret_NOMEM;
    }
    if (read_block_size > max_write_block_size) {
      read_block_size = max_write_block_size;
    }

    if (fopen_checked(outname, FOPEN_WB, &outfile)) {
      goto ExportBgen11_ret_OPEN_FAIL;
    }
    // bgen 1.1 header
    // note that \xxx character constants are interpreted in octal, so \24 is
    // decimal 20, etc.
    memcpy(writebuf, "\24\0\0\0\24\0\0\0", 8);
    memcpy(&(writebuf[8]), &variant_ct, 4);
    memcpy(&(writebuf[12]), &sample_ct, 4);
    memcpy(&(writebuf[16]), "bgen\5\0\0\0", 8);
    if (fwrite_checked(writebuf, 24, outfile)) {
      goto ExportBgen11_ret_WRITE_FAIL;
    }

    const uint32_t ref_allele_last = !(exportf_flags & kfExportfRefFirst);
    for (uint32_t tidx = 0; tidx < calc_thread_ct; ++tidx) {
      g_missing_acc1[tidx] = S_CAST(uintptr_t*, bigstack_alloc_raw(track_missing_cacheline_ct * kCacheline));
      g_bgen_geno_bufs[tidx] = S_CAST(uint16_t*, bigstack_alloc_raw(bgen_geno_cacheline_ct * kCacheline));
    }
    g_sample_ct = sample_ct;
    g_variant_include = variant_include;
    g_sample_include = sample_include;
    g_sample_include_cumulative_popcounts = sample_include_cumulative_popcounts;
    g_calc_thread_ct = calc_thread_ct;
    g_refalt1_select = refalt1_select;
    GetXymtStartAndEnd(cip, kChrOffsetY, &g_y_start, &g_y_end);
    g_ref_allele_last = ref_allele_last;
    g_cip = cip;
    g_error_ret = kPglRetSuccess;

    // 6 bytes present at start of every bgen-1.1 variant record
    memcpy(writebuf, &sample_ct, 4);
    memcpy(&(writebuf[4]), "\0", 2);

    // Main workflow:
    // 1. Set n=0, load/skip block 0
    //
    // 2. Spawn threads processing block n
    // 3. If n>0, write results for block (n-1)
    // 4. Increment n by 1
    // 5. Load/skip block n unless eof
    // 6. Join threads
    // 7. Goto step 2 unless eof
    //
    // 8. Write results for last block
    const uint32_t read_block_sizel = BitCtToWordCt(read_block_size);
    const uint32_t read_block_ct_m1 = (raw_variant_ct - 1) / read_block_size;
    uint32_t parity = 0;
    uint32_t read_block_idx = 0;
    uint32_t write_variant_uidx = 0;
    uint32_t chr_fo_idx = UINT32_MAX;
    uint32_t chr_end = 0;
    uint32_t chr_slen = 0;

    uint32_t prev_block_write_ct = 0;
    uint32_t variant_idx = 0;
    uint32_t is_last_block = 0;
    uint32_t cur_read_block_size = read_block_size;
    uint32_t pct = 0;
    uint32_t next_print_variant_idx = variant_ct / 100;
    logprintfww5("Writing %s ... ", outname);
    fputs("0%", stdout);
    fflush(stdout);
    uint32_t ref_allele_idx = 0;
    uint32_t alt1_allele_idx = 1;
    while (1) {
      uintptr_t cur_block_write_ct = 0;
      if (!is_last_block) {
        while (read_block_idx < read_block_ct_m1) {
          cur_block_write_ct = PopcountWords(&(variant_include[read_block_idx * read_block_sizel]), read_block_sizel);
          if (cur_block_write_ct) {
            break;
          }
          ++read_block_idx;
        }
        if (read_block_idx == read_block_ct_m1) {
          cur_read_block_size = raw_variant_ct - (read_block_idx * read_block_size);
          cur_block_write_ct = PopcountWords(&(variant_include[read_block_idx * read_block_sizel]), BitCtToWordCt(cur_read_block_size));
        }
        if (PgfiMultiread(variant_include, read_block_idx * read_block_size, read_block_idx * read_block_size + cur_read_block_size, cur_block_write_ct, pgfip)) {
          if (variant_idx) {
            JoinThreads2z(calc_thread_ct, 0, threads);
            g_cur_block_write_ct = 0;
            ErrorCleanupThreads2z(ExportBgen11Thread, calc_thread_ct, threads);
          }
          goto ExportBgen11_ret_READ_FAIL;
        }
      }
      if (variant_idx) {
        JoinThreads2z(calc_thread_ct, is_last_block, threads);
        reterr = g_error_ret;
        if (reterr) {
          if (!is_last_block) {
            g_cur_block_write_ct = 0;
            ErrorCleanupThreads2z(ExportBgen11Thread, calc_thread_ct, threads);
          }
          if (reterr == kPglRetMalformedInput) {
            logputs("\n");
            logerrputs("Error: Malformed .pgen file.\n");
          }
          goto ExportBgen11_ret_1;
        }
      }
      if (!is_last_block) {
        g_cur_block_write_ct = cur_block_write_ct;
        ComputeUidxStartPartition(variant_include, cur_block_write_ct, calc_thread_ct, read_block_idx * read_block_size, g_read_variant_uidx_starts);
        for (uint32_t tidx = 0; tidx < calc_thread_ct; ++tidx) {
          g_pgr_ptrs[tidx]->fi.block_base = pgfip->block_base;
          g_pgr_ptrs[tidx]->fi.block_offset = pgfip->block_offset;
        }
        is_last_block = (variant_idx + cur_block_write_ct == variant_ct);
        if (SpawnThreads2z(ExportBgen11Thread, calc_thread_ct, is_last_block, threads)) {
          goto ExportBgen11_ret_THREAD_CREATE_FAIL;
        }
      }
      parity = 1 - parity;
      if (variant_idx) {
        // write *previous* block results
        const unsigned char* compressed_data_iter = g_writebufs[parity];
        const uint32_t* variant_bytect_iter = g_variant_bytects[parity];
        for (uint32_t variant_bidx = 0; variant_bidx < prev_block_write_ct; ++variant_bidx, ++write_variant_uidx) {
          MovU32To1Bit(variant_include, &write_variant_uidx);
          if (write_variant_uidx >= chr_end) {
            do {
              ++chr_fo_idx;
              chr_end = cip->chr_fo_vidx_start[chr_fo_idx + 1];
            } while (write_variant_uidx >= chr_end);
            const uint32_t chr_idx = cip->chr_file_order[chr_fo_idx];
            const char* chr_name_end = chrtoa(cip, chr_idx, chr_buf);
            chr_slen = chr_name_end - chr_buf;
          }
          const char* cur_variant_id = variant_ids[write_variant_uidx];
          const uint32_t id_slen = strlen(cur_variant_id);
          memcpy(&(writebuf[6]), &id_slen, 4);
          // deliberately clobber top two bytes
          unsigned char* writebuf_iter = memcpyua(&(writebuf[8]), cur_variant_id, id_slen);
          *R_CAST(uint16_t*, writebuf_iter) = chr_slen;
          writebuf_iter = &(writebuf_iter[2]);
          writebuf_iter = memcpyua(writebuf_iter, chr_buf, chr_slen);
          writebuf_iter = memcpyua(writebuf_iter, &(variant_bps[write_variant_uidx]), 4);
          uintptr_t variant_allele_idx_base = write_variant_uidx * 2;
          if (variant_allele_idxs) {
            variant_allele_idx_base = variant_allele_idxs[write_variant_uidx];
          }
          const char* const* cur_alleles = &(allele_storage[variant_allele_idx_base]);
          if (refalt1_select) {
            ref_allele_idx = refalt1_select[write_variant_uidx * 2];
            alt1_allele_idx = refalt1_select[write_variant_uidx * 2 + 1];
          }
          const char* first_allele;
          const char* second_allele;
          if (ref_allele_last) {
            first_allele = cur_alleles[alt1_allele_idx];
            second_allele = cur_alleles[ref_allele_idx];
          } else {
            first_allele = cur_alleles[ref_allele_idx];
            second_allele = cur_alleles[alt1_allele_idx];
          }
          uint32_t allele_slen = strlen(first_allele);
          writebuf_iter = memcpyua(writebuf_iter, &allele_slen, 4);
          writebuf_iter = memcpyua(writebuf_iter, first_allele, allele_slen);
          allele_slen = strlen(second_allele);
          writebuf_iter = memcpyua(writebuf_iter, &allele_slen, 4);
          writebuf_iter = memcpyua(writebuf_iter, second_allele, allele_slen);
          const uint32_t cur_variant_bytect = *variant_bytect_iter++;
          writebuf_iter = memcpyua(writebuf_iter, &cur_variant_bytect, 4);
          writebuf_iter = memcpyua(writebuf_iter, compressed_data_iter, cur_variant_bytect);
          compressed_data_iter = &(compressed_data_iter[bgen_compressed_buf_max]);
          if (fwrite_checked(writebuf, writebuf_iter - writebuf, outfile)) {
            if (variant_idx < variant_ct) {
              JoinThreads2z(calc_thread_ct, is_last_block, threads);
              if (!is_last_block) {
                g_cur_block_write_ct = 0;
                ErrorCleanupThreads2z(ExportBgen11Thread, calc_thread_ct, threads);
              }
            }
            goto ExportBgen11_ret_WRITE_FAIL;
          }
        }
      }
      if (variant_idx == variant_ct) {
        break;
      }
      if (variant_idx >= next_print_variant_idx) {
        if (pct > 10) {
          putc_unlocked('\b', stdout);
        }
        pct = (variant_idx * 100LLU) / variant_ct;
        printf("\b\b%u%%", pct++);
        fflush(stdout);
        next_print_variant_idx = (pct * S_CAST(uint64_t, variant_ct)) / 100;
      }
      ++read_block_idx;
      prev_block_write_ct = cur_block_write_ct;
      variant_idx += cur_block_write_ct;
      pgfip->block_base = main_loadbufs[parity];
    }
    if (fclose_null(&outfile)) {
      goto ExportBgen11_ret_WRITE_FAIL;
    }
    if (pct > 10) {
      putc_unlocked('\b', stdout);
    }
    fputs("\b\b", stdout);
    logprintf("done.\n");
    const uint32_t sample_ctav = acc1_vec_ct * kBitsPerVec;
    const uintptr_t acc32_offset = acc1_vec_ct * (13 * k1LU * kWordsPerVec);
    uint32_t* scrambled_missing_cts = R_CAST(uint32_t*, &(g_missing_acc1[0][acc32_offset]));
    for (uint32_t tidx = 1; tidx < calc_thread_ct; ++tidx) {
      const uint32_t* thread_scrambled_missing_cts = R_CAST(uint32_t*, &(g_missing_acc1[tidx][acc32_offset]));
      for (uint32_t uii = 0; uii < sample_ctav; ++uii) {
        scrambled_missing_cts[uii] += thread_scrambled_missing_cts[uii];
      }
    }
    for (uint32_t sample_idx = 0; sample_idx < sample_ct; ++sample_idx) {
      const uint32_t scrambled_idx = VcountScramble1(sample_idx);
      sample_missing_geno_cts[sample_idx] = scrambled_missing_cts[scrambled_idx];
    }
  }
  while (0) {
  ExportBgen11_ret_NOMEM:
    reterr = kPglRetNomem;
    break;
  ExportBgen11_ret_OPEN_FAIL:
    reterr = kPglRetOpenFail;
    break;
  ExportBgen11_ret_READ_FAIL:
    reterr = kPglRetReadFail;
    break;
  ExportBgen11_ret_WRITE_FAIL:
    reterr = kPglRetWriteFail;
    break;
#ifdef __LP64__
  ExportBgen11_ret_INCONSISTENT_INPUT:
    reterr = kPglRetInconsistentInput;
    break;
#endif
  ExportBgen11_ret_THREAD_CREATE_FAIL:
    reterr = kPglRetThreadCreateFail;
    break;
  }
 ExportBgen11_ret_1:
  if (g_libdeflate_compressors) {
    for (uint32_t tidx = 0; tidx < max_thread_ct; ++tidx) {
      if (!g_libdeflate_compressors[tidx]) {
        break;
      }
      libdeflate_free_compressor(g_libdeflate_compressors[tidx]);
    }
    g_libdeflate_compressors = nullptr;
  }
  fclose_cond(outfile);
  BigstackReset(bigstack_mark);
  return reterr;
}

// For constant-ploidy cases.  Writes up to 7 bytes past the end.
// For what it's worth, bgen_geno_buf_iter is always word-aligned on entry (8
// bytes after beginning of buffer, which is at the start of a cacheline), and
// it's easy to force vector-alignment if it would be useful.
unsigned char* FillBgen13PloidyAndMissingness(const uintptr_t* genovec, const uintptr_t* dosage_present, uintptr_t ploidy, uint32_t sample_ct, uint32_t dosage_ct, unsigned char* bgen_geno_buf_iter) {
  uint64_t* sample_ploidy_and_missingness_alias = R_CAST(uint64_t*, bgen_geno_buf_iter);
  const uint64_t ploidy_u64 = ploidy * kMask0101;
  const uint32_t sample_ct8 = DivUp(sample_ct, 8);
  const uint16_t* genovec_alias = R_CAST(const uint16_t*, genovec);
  if (!dosage_ct) {
    for (uint32_t write_widx = 0; write_widx < sample_ct8; ++write_widx) {
      const uint64_t cur_geno8 = genovec_alias[write_widx];
      uint64_t cur_missing8 = cur_geno8 & (cur_geno8 >> 1);
#ifdef USE_AVX2
      // 0,2,4,6...14 -> 7,15,23,...,63
      // todo: try inverse-movemask
      cur_missing8 = _pext_u64(cur_missing8 & 0x5555, 8 * kMask1111);
#else
      // 0,2,4,6,8,10,12,14 -> 0,2,4,6,32,34,36,38
      cur_missing8 = (cur_missing8 | (cur_missing8 << 24)) & 0x5500000055LLU;
      // Multiply by number with bits 7,13,19,25 set, then mask out all but the
      // bits we want.  There are potential carries at bits 13, 19, 25, 45, 51,
      // and 57, but none of them hurt us.
      cur_missing8 = (cur_missing8 * 0x2082080) & (0x8080808080808080LLU);
#endif
      sample_ploidy_and_missingness_alias[write_widx] = cur_missing8 + ploidy_u64;
    }
  } else {
    // don't bother with word-based 32-bit code because this case becomes
    // more annoying
    const unsigned char* dosage_present_alias = R_CAST(const unsigned char*, dosage_present);
    for (uint32_t write_widx = 0; write_widx < sample_ct8; ++write_widx) {
      const uint64_t cur_geno8 = genovec_alias[write_widx];
      uint64_t cur_dosage_missing8 = ~dosage_present_alias[write_widx];
      uint64_t cur_hardcall_missing8 = cur_geno8 & (cur_geno8 >> 1);
#ifdef USE_AVX2
      // 0,2,4,6...14 -> 7,15,23,...,63
      cur_hardcall_missing8 = _pext_u64(cur_hardcall_missing8 & 0x5555, 8 * kMask1111);
      cur_dosage_missing8 = _pext_u64(cur_dosage_missing8, 0x80 * kMask0101);
      const uint64_t cur_missing8 = cur_hardcall_missing8 & cur_dosage_missing8;
#else
      // 0,2,4,6,8,10,12,14 -> 0,2,4,6,32,34,36,38
      cur_hardcall_missing8 = (cur_hardcall_missing8 | (cur_hardcall_missing8 << 24)) & 0x5500000055LLU;
      // -> 0,8,16,24,32,40,48,56, with extraneous bits set
      cur_hardcall_missing8 = cur_hardcall_missing8 * 0x41041;
      // 0,1,2,3,4,5,6,7 -> 0,8,16,24,32,40,48,56, with extraneous bits set
      // this operation also appears in GflagsVfilter(), may want to wrap it
      cur_dosage_missing8 = ((cur_dosage_missing8 & 0xfe) * 0x2040810204080LLU) | (cur_dosage_missing8 & 1);
      const uint64_t cur_missing8 = ((cur_hardcall_missing8 & cur_dosage_missing8) & 0x101010101010101LLU) << 7;
#endif
      sample_ploidy_and_missingness_alias[write_widx] = cur_missing8 + ploidy_u64;
    }
  }
  return &(bgen_geno_buf_iter[sample_ct]);
}

uint32_t NoFemaleMissing(const uintptr_t* genovec, const uintptr_t* dosage_present, const uintptr_t* sex_female, uint32_t sample_ctl2, uint32_t dosage_ct) {
  if (dosage_ct) {
    const uint32_t sample_ctl = DivUp(sample_ctl2, 2);
    if (!IntersectionIsEmpty(sex_female, dosage_present, sample_ctl)) {
      return 0;
    }
  }
  const Halfword* sex_female_alias = R_CAST(const Halfword*, sex_female);
  for (uint32_t widx = 0; widx < sample_ctl2; ++widx) {
    const uintptr_t geno_word = genovec[widx];
    const uintptr_t cur_female_01 = UnpackHalfwordToWord(sex_female_alias[widx]);
    if (geno_word & (geno_word >> 1) & cur_female_01) {
      return 0;
    }
  }
  return 1;
}

static unsigned char** g_thread_wkspaces = nullptr;
static uintptr_t** g_phasepresents = nullptr;
static uintptr_t** g_phaseinfos = nullptr;
static uintptr_t** g_dphase_presents = nullptr;
static SDosage** g_dphase_deltas = nullptr;
static uintptr_t* g_sex_female_collapsed = nullptr;

static uint32_t g_bgen_bit_precision = 0;

// Precomputed 1-genotype-at-a-time tables (4 entries)
static uint16_t* g_bgen_diploid_basic_table8 = nullptr;
static uint32_t* g_bgen_diploid_basic_table16 = nullptr;
static unsigned char* g_bgen_haploid_basic_table8 = nullptr;
static uint16_t* g_bgen_haploid_basic_table16 = nullptr;

// Precomputed 4-genotype-at-a-time tables (256 entries; multiply index by two
// and copy two values at a time for table16).
static uint64_t* g_bgen_diploid_hardcall_table8 = nullptr;
static uint64_t* g_bgen_diploid_hardcall_table16 = nullptr;

// Precomputed 1-genotype-at-a-time tables (16 entries: phasepresent = bit 2,
// phaseinfo = bit 3)
static uint16_t* g_bgen_diploid_phased_hardcall_table8 = nullptr;
static uint32_t* g_bgen_diploid_phased_hardcall_table16 = nullptr;

// 4-genotype-at-a-time tables.
static uint32_t* g_bgen_haploid_hardcall_table8 = nullptr;
static uint64_t* g_bgen_haploid_hardcall_table16 = nullptr;

BoolErr ConstructBgen13LookupTables(uint32_t exportf_bits) {
  const uint32_t max_val = (1U << exportf_bits) - 1;
  const uint32_t half_val_roundeven = ((max_val + 1) / 2) & (~1);
  if (exportf_bits <= 8) {
    // can conditionally skip some of these tables, but whatever
    if (bigstack_alloc_u16(4, &g_bgen_diploid_basic_table8) ||
        bigstack_alloc_u64(256, &g_bgen_diploid_hardcall_table8) ||
        bigstack_alloc_u16(16, &g_bgen_diploid_phased_hardcall_table8) ||
        bigstack_alloc_uc(4, &g_bgen_haploid_basic_table8) ||
        bigstack_alloc_u32(256, &g_bgen_haploid_hardcall_table8)) {
      return 1;
    }
    // reference allele is exported as second allele so default import and
    // export settings work together
    // (yeah, this will make multiallelic case annoying)
    // unphased diploid:
    //   [0] = 0, 0
    //   [1] = 0, max
    //   [2] = max, 0
    //   [3] = 0, 0
    g_bgen_diploid_basic_table8[0] = 0;
    g_bgen_diploid_basic_table8[1] = max_val << 8;
    g_bgen_diploid_basic_table8[2] = max_val;
    g_bgen_diploid_basic_table8[3] = 0;
    uint64_t* write64_iter = g_bgen_diploid_hardcall_table8;
    for (uint32_t uii = 0; uii < 4; ++uii) {
      const uint64_t entry3 = S_CAST(uint64_t, g_bgen_diploid_basic_table8[uii]) << 48;
      for (uint32_t ujj = 0; ujj < 4; ++ujj) {
        const uint64_t entry23 = entry3 | (S_CAST(uint64_t, g_bgen_diploid_basic_table8[ujj]) << 32);
        for (uint32_t ukk = 0; ukk < 4; ++ukk) {
          const uint64_t entry123 = entry23 | (S_CAST(uint64_t, g_bgen_diploid_basic_table8[ukk]) << 16);
          for (uint32_t umm = 0; umm < 4; ++umm) {
            *write64_iter++ = entry123 | g_bgen_diploid_basic_table8[umm];
          }
        }
      }
    }
    // phased diploid:
    //   [0] = 0, 0
    //   [1] unphased = half, half
    //   [1] phased, phaseinfo clear = 0, max
    //   [1] phased, phaseinfo set = max, 0
    //   [2] = max, max
    //   [3] = 0, 0
    g_bgen_diploid_phased_hardcall_table8[0] = 0;
    g_bgen_diploid_phased_hardcall_table8[1] = half_val_roundeven * 257;
    g_bgen_diploid_phased_hardcall_table8[2] = max_val * 257;
    g_bgen_diploid_phased_hardcall_table8[3] = 0;
    // might need to defend against garbage phaseinfo
    // cheap to defend against garbage phasepresent
    memcpy(&(g_bgen_diploid_phased_hardcall_table8[4]), g_bgen_diploid_phased_hardcall_table8, 4 * sizeof(int16_t));
    memcpy(&(g_bgen_diploid_phased_hardcall_table8[8]), g_bgen_diploid_phased_hardcall_table8, 8 * sizeof(int16_t));
    g_bgen_diploid_phased_hardcall_table8[5] = max_val << 8;
    g_bgen_diploid_phased_hardcall_table8[13] = max_val;

    // haploid:
    //   [0] = 0
    //   [1] = half, may as well round-to-even
    //   [2] = max
    //   [3] = 0
    g_bgen_haploid_basic_table8[0] = 0;
    g_bgen_haploid_basic_table8[1] = half_val_roundeven;
    g_bgen_haploid_basic_table8[2] = max_val;
    g_bgen_haploid_basic_table8[3] = 0;
    uint32_t* write32_iter = g_bgen_haploid_hardcall_table8;
    for (uint32_t uii = 0; uii < 4; ++uii) {
      const uint32_t entry3 = S_CAST(uint32_t, g_bgen_haploid_basic_table8[uii]) << 24;
      for (uint32_t ujj = 0; ujj < 4; ++ujj) {
        const uint32_t entry23 = entry3 | (S_CAST(uint32_t, g_bgen_haploid_basic_table8[ujj]) << 16);
        for (uint32_t ukk = 0; ukk < 4; ++ukk) {
          const uint32_t entry123 = entry23 | (S_CAST(uint32_t, g_bgen_haploid_basic_table8[ukk]) << 8);
          for (uint32_t umm = 0; umm < 4; ++umm) {
            *write32_iter++ = entry123 | g_bgen_haploid_basic_table8[umm];
          }
        }
      }
    }
  } else {
    // (yeah, a template function makes sense here...)
    if (bigstack_alloc_u32(4, &g_bgen_diploid_basic_table16) ||
        bigstack_alloc_u64(512, &g_bgen_diploid_hardcall_table16) ||
        bigstack_alloc_u32(16, &g_bgen_diploid_phased_hardcall_table16) ||
        bigstack_alloc_u16(4, &g_bgen_haploid_basic_table16) ||
        bigstack_alloc_u64(256, &g_bgen_haploid_hardcall_table16)) {
      return 1;
    }
    g_bgen_diploid_basic_table16[0] = 0;
    g_bgen_diploid_basic_table16[1] = max_val << 16;
    g_bgen_diploid_basic_table16[2] = max_val;
    g_bgen_diploid_basic_table16[3] = 0;
    uint64_t* write64_iter = g_bgen_diploid_hardcall_table16;
    for (uint32_t uii = 0; uii < 4; ++uii) {
      const uint64_t entry3 = S_CAST(uint64_t, g_bgen_diploid_basic_table16[uii]) << 32;
      for (uint32_t ujj = 0; ujj < 4; ++ujj) {
        const uint64_t entry23 = entry3 | g_bgen_diploid_basic_table16[ujj];
        for (uint32_t ukk = 0; ukk < 4; ++ukk) {
          const uint64_t entry1 = S_CAST(uint64_t, g_bgen_diploid_basic_table16[ukk]) << 32;
          for (uint32_t umm = 0; umm < 4; ++umm) {
            *write64_iter++ = entry1 | g_bgen_diploid_basic_table16[umm];
            *write64_iter++ = entry23;
          }
        }
      }
    }
    g_bgen_diploid_phased_hardcall_table16[0] = 0;
    g_bgen_diploid_phased_hardcall_table16[1] = half_val_roundeven * 65537;
    g_bgen_diploid_phased_hardcall_table16[2] = max_val * 65537;
    g_bgen_diploid_phased_hardcall_table16[3] = 0;
    memcpy(&(g_bgen_diploid_phased_hardcall_table16[4]), g_bgen_diploid_phased_hardcall_table16, 4 * sizeof(int32_t));
    memcpy(&(g_bgen_diploid_phased_hardcall_table16[8]), g_bgen_diploid_phased_hardcall_table16, 8 * sizeof(int32_t));
    g_bgen_diploid_phased_hardcall_table16[5] = max_val << 16;
    g_bgen_diploid_phased_hardcall_table16[13] = max_val;

    g_bgen_haploid_basic_table16[0] = 0;
    g_bgen_haploid_basic_table16[1] = half_val_roundeven;
    g_bgen_haploid_basic_table16[2] = max_val;
    g_bgen_haploid_basic_table16[3] = 0;
    write64_iter = g_bgen_haploid_hardcall_table16;
    for (uint32_t uii = 0; uii < 4; ++uii) {
      const uint64_t entry3 = S_CAST(uint64_t, g_bgen_haploid_basic_table16[uii]) << 48;
      for (uint32_t ujj = 0; ujj < 4; ++ujj) {
        const uint64_t entry23 = entry3 | (S_CAST(uint64_t, g_bgen_haploid_basic_table16[ujj]) << 32);
        for (uint32_t ukk = 0; ukk < 4; ++ukk) {
          const uint64_t entry123 = entry23 | (S_CAST(uint64_t, g_bgen_haploid_basic_table16[ukk]) << 16);
          for (uint32_t umm = 0; umm < 4; ++umm) {
            *write64_iter++ = entry123 | g_bgen_haploid_basic_table16[umm];
          }
        }
      }
    }
  }
  g_bgen_bit_precision = exportf_bits;
  return 0;
}

THREAD_FUNC_DECL ExportBgen13Thread(void* arg) {
  const uintptr_t tidx = R_CAST(uintptr_t, arg);
  PgenReader* pgrp = g_pgr_ptrs[tidx];
  uintptr_t* genovec = g_genovecs[tidx];
  const uint32_t sample_ct = g_sample_ct;
  const uint32_t acc1_vec_ct = BitCtToVecCt(sample_ct);
  const uint32_t acc4_vec_ct = acc1_vec_ct * 4;
  const uint32_t acc8_vec_ct = acc1_vec_ct * 8;
  uintptr_t* missing_acc1 = g_missing_acc1[tidx];
  uintptr_t* missing_acc4 = &(missing_acc1[acc1_vec_ct * kWordsPerVec]);
  uintptr_t* missing_acc8 = &(missing_acc4[acc4_vec_ct * kWordsPerVec]);
  uintptr_t* missing_acc32 = &(missing_acc8[acc8_vec_ct * kWordsPerVec]);
  uintptr_t* phasepresent = nullptr;
  uintptr_t* phaseinfo = nullptr;
  if (g_phasepresents) {
    phasepresent = g_phasepresents[tidx];
    phaseinfo = g_phaseinfos[tidx];
  }
  uintptr_t* dosage_present = g_dosage_presents? g_dosage_presents[tidx] : nullptr;
  Dosage* dosage_main = dosage_present? g_dosage_mains[tidx] : nullptr;
  uintptr_t* dphase_present = g_dphase_presents? g_dphase_presents[tidx] : nullptr;
  SDosage* dphase_delta = dphase_present? g_dphase_deltas[tidx] : nullptr;
  // Note that we may write up to 12 bytes past the end
  unsigned char* uncompressed_bgen_geno_buf = g_thread_wkspaces[tidx];
  struct libdeflate_compressor* compressor = g_libdeflate_compressors? g_libdeflate_compressors[tidx] : nullptr;
  const uint32_t zst_level = g_zst_level;
  const uintptr_t* variant_include = g_variant_include;
  const ChrInfo* cip = g_cip;
  const uintptr_t* sample_include = g_sample_include;
  const uint32_t* sample_include_cumulative_popcounts = g_sample_include_cumulative_popcounts;
  const uintptr_t* sex_male_collapsed = g_sex_male_collapsed;
  const uintptr_t* sex_female_collapsed = g_sex_female_collapsed;
  /*
  const unsigned char* bgen_haploid_basic_table8 = g_bgen_haploid_basic_table8;
  const uint16_t* bgen_haploid_basic_table16 = g_bgen_haploid_basic_table16;
  const uint16_t* bgen_diploid_basic_table8 = g_bgen_diploid_basic_table8;
  const uint32_t* bgen_diploid_basic_table16 = g_bgen_diploid_basic_table16;
  */
  const uint64_t* bgen_diploid_hardcall_table8 = g_bgen_diploid_hardcall_table8;
  const uint64_t* bgen_diploid_hardcall_table16 = g_bgen_diploid_hardcall_table16;
  const uint16_t* bgen_diploid_phased_hardcall_table8 = g_bgen_diploid_phased_hardcall_table8;
  const uint32_t* bgen_diploid_phased_hardcall_table16 = g_bgen_diploid_phased_hardcall_table16;
  /*
  const uint32_t* bgen_haploid_hardcall_table8 = g_bgen_haploid_hardcall_table8;
  const uint64_t* bgen_haploid_hardcall_table16 = g_bgen_haploid_hardcall_table16;
  */
  const uint32_t calc_thread_ct = g_calc_thread_ct;
  const uint32_t sample_ctl = BitCtToWordCt(sample_ct);
  const uint32_t sample_ctl2 = QuaterCtToWordCt(sample_ct);
  const uint32_t sample_ctl2_m1 = sample_ctl2 - 1;
  const uint32_t sample_ct4 = DivUp(sample_ct, 4);
  const uint32_t bit_precision = g_bgen_bit_precision;
  const uint32_t max_output_val = (1U << bit_precision) - 1;
  const uint32_t bgen_compressed_buf_max = g_bgen_compressed_buf_max;
  const AltAlleleCt* refalt1_select = g_refalt1_select;
  uint32_t chr_fo_idx = UINT32_MAX;  // deliberate overflow
  uint32_t chr_end = 0;
  uint32_t is_x = 0;
  uint32_t is_y = 0;
  uint32_t cur_y = 0;

  uint32_t is_haploid = 0;  // includes chrX
  // Unlike the VCF/BCF export case,
  // * we do not save heterozygous haploid calls as ploidy=2 unless there is
  //   also phase information.
  // * chrY female (but not unknown-sex) ploidy is 0 when genotype is missing.
  const uint32_t male_ct = PopcountWords(sex_male_collapsed, sample_ctl);
  const uint32_t female_ct = PopcountWords(sex_female_collapsed, sample_ctl);
  const uint32_t x_code = (male_ct != sample_ct)? cip->xymt_codes[kChrOffsetX] : UINT32_MAXM1;
  const uint32_t y_code = female_ct? cip->xymt_codes[kChrOffsetY] : UINT32_MAXM1;
  const uint32_t ref_allele_last = g_ref_allele_last;
  uint32_t vidx_rem15 = 15;
  uint32_t vidx_rem255d15 = 17;
  uint32_t ref_allele_idx = 0;
  uint32_t parity = 0;
  ZeroWArr(acc1_vec_ct * kWordsPerVec * 45, missing_acc1);
  while (1) {
    const uint32_t is_last_block = g_is_last_thread_block;
    const uintptr_t cur_block_write_ct = g_cur_block_write_ct;
    uint32_t write_idx = (tidx * cur_block_write_ct) / calc_thread_ct;
    const uint32_t write_idx_end = ((tidx + 1) * cur_block_write_ct) / calc_thread_ct;
    unsigned char* writebuf_iter = &(g_writebufs[parity][write_idx * S_CAST(uintptr_t, bgen_compressed_buf_max)]);
    // [2n] = 4 + compressed_len, [2n + 1] = uncompressed_len
    uint32_t* variant_bytect_iter = &(g_variant_bytects[parity][2 * write_idx]);
    uint32_t variant_uidx = g_read_variant_uidx_starts[tidx];
    for (; write_idx < write_idx_end; ++write_idx, ++variant_uidx) {
      MovU32To1Bit(variant_include, &variant_uidx);
      if (variant_uidx >= chr_end) {
        do {
          ++chr_fo_idx;
          chr_end = cip->chr_fo_vidx_start[chr_fo_idx + 1];
        } while (variant_uidx >= chr_end);
        const uint32_t chr_idx = cip->chr_file_order[chr_fo_idx];
        is_y = (chr_idx == y_code);
        is_haploid = IsSet(cip->haploid_mask, chr_idx);
        is_x = 0;
        if (chr_idx == x_code) {
          if (male_ct) {
            is_x = 1;
          } else {
            is_haploid = 0;
          }
        }
        cur_y = 0;
      }
      if (refalt1_select) {
        ref_allele_idx = refalt1_select[variant_uidx * 2];
      }
      // todo: multiallelic cases
      // (note that multiallelic variants should always be stored using the
      // phased format; just set both sides equal when the call is actually
      // unphased.  this way there's no O([allele count]^2) bloat problem.)
      uint32_t phasepresent_ct;
      uint32_t dosage_ct;
      uint32_t dphase_ct;
      PglErr reterr = PgrGetDp(sample_include, sample_include_cumulative_popcounts, sample_ct, variant_uidx, pgrp, genovec, phasepresent, phaseinfo, &phasepresent_ct, dosage_present, dosage_main, &dosage_ct, dphase_present, dphase_delta, &dphase_ct);
      if (reterr) {
        g_error_ret = reterr;
        break;
      }
      // note that this is inverted from bgen-1.1
      if (ref_allele_idx + ref_allele_last != 1) {
        GenovecInvertUnsafe(sample_ct, genovec);
        if (phasepresent_ct) {
          BitvecInvert(sample_ctl, phaseinfo);
        }
        if (dosage_ct) {
          BiallelicDosage16Invert(dosage_ct, dosage_main);
          if (dphase_ct) {
            BiallelicDphase16Invert(dphase_ct, dphase_delta);
          }
        }
      }
      unsigned char* bgen_geno_buf_iter = uncompressed_bgen_geno_buf;
      // 4 bytes: # of samples
      // 2 bytes: # of alleles
      // 1 byte: minimum ploidy
      // 1 byte: maximum ploidy
      // sample_ct bytes: high bit = missing, low bits = ploidy
      // 1 byte: is_phased
      // 1 byte: bit_precision

      bgen_geno_buf_iter = memcpyua(bgen_geno_buf_iter, &sample_ct, 4);
      if (is_y) {
        // All-nonfemales case treated as haploid general case.
        // The difference here is that missing female calls are saved as
        // ploidy 0; this basically forces us to write a single value at
        // a time.
        cur_y = !NoFemaleMissing(genovec, dosage_present, sex_female_collapsed, sample_ctl2, dosage_ct);
      }
      if (dphase_ct && (bit_precision < 15)) {
        // Theoretically possible for all dphase_delta values to be too small
        // to ever make left dosage != right dosage.  If so, allow unphased
        // output.
        uint32_t sample_uidx = 0;
        uint32_t dphase_idx = 0;
        uint32_t dosage_idx = 0;
        for (; dosage_idx < dosage_ct; ++dosage_idx, ++sample_uidx) {
          MovU32To1Bit(dosage_present, &sample_uidx);
          if (IsSet(dphase_present, sample_uidx)) {
            const uint32_t cur_dosage = dosage_main[dosage_idx];
            const int32_t cur_dphase_delta_val = dphase_delta[dphase_idx++];
            const uint32_t left_raw = (cur_dosage + cur_dphase_delta_val) >> 1;
            const uint32_t right_raw = (cur_dosage - cur_dphase_delta_val) >> 1;
            const uint32_t left_output_preshift = left_raw * max_output_val + kDosage4th;
            const uint32_t right_output_preshift = right_raw * max_output_val + kDosage4th;
            if ((left_output_preshift ^ right_output_preshift) / kDosageMid) {
              break;
            }
          }
        }
        if (dosage_idx >= dosage_ct) {
          dphase_ct = 0;
        }
      }
      const uint32_t use_phased_format = phasepresent_ct || dphase_ct;
      if (use_phased_format && dosage_ct) {
        if (!phasepresent_ct) {
          ZeroWArr(sample_ctl, phasepresent);
        } else if (!dphase_ct) {
          ZeroWArr(sample_ctl, dphase_present);
        }
      }
      if ((!is_haploid) || (phasepresent_ct == sample_ct) || (dphase_ct && UnionIsFull(phasepresent, dphase_present, sample_ct))) {
        // 2 alleles, min ploidy == max ploidy == 2
        // (this includes the chrX no-sex-info case)
        // dosages can be patched in after the fact
        *R_CAST(uint32_t*, bgen_geno_buf_iter) = 0x2020002;
        bgen_geno_buf_iter = &(bgen_geno_buf_iter[4]);
        bgen_geno_buf_iter = FillBgen13PloidyAndMissingness(genovec, dosage_present, 2, sample_ct, dosage_ct, bgen_geno_buf_iter);
        const unsigned char* genovec_alias = R_CAST(unsigned char*, genovec);
        if (!use_phased_format) {
          *bgen_geno_buf_iter++ = 0;
          *bgen_geno_buf_iter++ = bit_precision;
          uint64_t* genodata_alias_iter = R_CAST(uint64_t*, bgen_geno_buf_iter);
          if (bit_precision <= 8) {
            for (uint32_t geno_byte_idx = 0; geno_byte_idx < sample_ct4; ++geno_byte_idx) {
              const unsigned char cur_geno4 = genovec_alias[geno_byte_idx];
              genodata_alias_iter[geno_byte_idx] = bgen_diploid_hardcall_table8[cur_geno4];
            }
          } else {
            // 9..16
            for (uint32_t geno_byte_idx = 0; geno_byte_idx < sample_ct4; ++geno_byte_idx) {
              const unsigned char cur_geno4 = genovec_alias[geno_byte_idx];
              *genodata_alias_iter++ = bgen_diploid_hardcall_table16[2 * cur_geno4];
              *genodata_alias_iter++ = bgen_diploid_hardcall_table16[2 * cur_geno4 + 1];
            }
          }
        } else {
          // use_phased_format
          *bgen_geno_buf_iter++ = 1;
          *bgen_geno_buf_iter++ = bit_precision;
          uint32_t widx = 0;
          uint32_t loop_len = kBitsPerWordD2;
          if (bit_precision <= 8) {
            uint16_t* genodata_alias_iter = R_CAST(uint16_t*, bgen_geno_buf_iter);
            while (1) {
              if (widx >= sample_ctl2_m1) {
                if (widx > sample_ctl2_m1) {
                  break;
                }
                loop_len = ModNz(sample_ct, kBitsPerWordD2);
              }
              uintptr_t geno_word = genovec[widx];
              // probable todo: fast path for phasepresent_hw == 0
              uintptr_t phaseword = UnpackHalfwordToWord(R_CAST(Halfword*, phasepresent)[widx]) | UnpackHalfwordToWordShift1(R_CAST(Halfword*, phaseinfo)[widx]);
              for (uint32_t uii = 0; uii < loop_len; ++uii) {
                // could halve the loop length and shuffle geno_word and
                // phaseword together, with single right-shift-4 each
                // iteration
                *genodata_alias_iter++ = bgen_diploid_phased_hardcall_table8[(geno_word & 3) | ((phaseword & 3) * 4)];
                geno_word >>= 2;
                phaseword >>= 2;
              }
              ++widx;
            }
          } else {
            uint32_t* genodata_alias_iter = R_CAST(uint32_t*, bgen_geno_buf_iter);
            while (1) {
              if (widx >= sample_ctl2_m1) {
                if (widx > sample_ctl2_m1) {
                  break;
                }
                loop_len = ModNz(sample_ct, kBitsPerWordD2);
              }
              uintptr_t geno_word = genovec[widx];
              uintptr_t phaseword = UnpackHalfwordToWord(R_CAST(Halfword*, phasepresent)[widx]) | UnpackHalfwordToWordShift1(R_CAST(Halfword*, phaseinfo)[widx]);
              for (uint32_t uii = 0; uii < loop_len; ++uii) {
                *genodata_alias_iter++ = bgen_diploid_phased_hardcall_table16[(geno_word & 3) | ((phaseword & 3) * 4)];
                geno_word >>= 2;
                phaseword >>= 2;
              }
              ++widx;
            }
          }
        }
        if (dosage_ct) {
          uint32_t sample_uidx = 0;
          if (!use_phased_format) {
            // first value is P(geno=2), second value is P(geno=1)
            if (bit_precision <= 8) {
              for (uint32_t dosage_idx = 0; dosage_idx < dosage_ct; ++dosage_idx, ++sample_uidx) {
                // multiply by e.g. 255/16384 and round
                // there are multiple ways to interpret "round to even" here,
                // so I give up and just round 0.5 up (especially since it
                // should be very rare).
                MovU32To1Bit(dosage_present, &sample_uidx);
                // note that this needs to be changed to uint64_t if internal
                // dosage representation is no longer 16-bit
                const uint32_t cur_dosage = dosage_main[dosage_idx];
                uint32_t output_prob2 = 0;
                uint32_t output_prob1;
                if (cur_dosage > kDosageMid) {
                  output_prob2 = ((cur_dosage - kDosageMid) * max_output_val + kDosage4th) / kDosageMid;
                  output_prob1 = max_output_val - output_prob2;
                } else {
                  output_prob1 = (cur_dosage * max_output_val + kDosage4th) / kDosageMid;
                }
                bgen_geno_buf_iter[sample_uidx * 2] = output_prob2;
                bgen_geno_buf_iter[sample_uidx * 2 + 1] = output_prob1;
              }
            } else {
              uint16_t* genodata_alias = R_CAST(uint16_t*, bgen_geno_buf_iter);
              for (uint32_t dosage_idx = 0; dosage_idx < dosage_ct; ++dosage_idx, ++sample_uidx) {
                MovU32To1Bit(dosage_present, &sample_uidx);
                const uint32_t cur_dosage = dosage_main[dosage_idx];
                uint32_t output_prob2 = 0;
                uint32_t output_prob1;
                if (cur_dosage > kDosageMid) {
                  output_prob2 = ((cur_dosage - kDosageMid) * max_output_val + kDosage4th) / kDosageMid;
                  output_prob1 = max_output_val - output_prob2;
                } else {
                  output_prob1 = (cur_dosage * max_output_val + kDosage4th) / kDosageMid;
                }
                genodata_alias[sample_uidx * 2] = output_prob2;
                genodata_alias[sample_uidx * 2 + 1] = output_prob1;
              }
            }
          } else {
            // phased format, need to take hphase and/or dphase into account
            uint16_t* genodata_alias = nullptr;
            if (bit_precision > 8) {
              genodata_alias = R_CAST(uint16_t*, bgen_geno_buf_iter);
            }
            uint32_t dphase_idx = 0;
            for (uint32_t dosage_idx = 0; dosage_idx < dosage_ct; ++dosage_idx, ++sample_uidx) {
              MovU32To1Bit(dosage_present, &sample_uidx);
              const uint32_t cur_dosage = dosage_main[dosage_idx];
              uint32_t output_prob1;
              uint32_t output_prob2;
              if (IsSet(dphase_present, sample_uidx)) {
                const int32_t cur_dphase_delta_val = dphase_delta[dphase_idx++];
                const uint32_t left_raw = (cur_dosage + cur_dphase_delta_val) >> 1;
                const uint32_t right_raw = (cur_dosage - cur_dphase_delta_val) >> 1;
                output_prob1 = (left_raw * max_output_val + kDosage4th) / kDosageMid;
                output_prob2 = (right_raw * max_output_val + kDosage4th) / kDosageMid;
              } else if (IsSet(phasepresent, sample_uidx)) {
                if (cur_dosage > kDosageMid) {
                  output_prob1 = ((cur_dosage - kDosageMid) * max_output_val + kDosage4th) / kDosageMid;
                  output_prob2 = max_output_val;
                } else {
                  output_prob1 = 0;
                  output_prob2 = (cur_dosage * max_output_val + kDosage4th) / kDosageMid;
                }
                if (IsSet(phaseinfo, sample_uidx)) {
                  const uint32_t tmpval = output_prob1;
                  output_prob1 = output_prob2;
                  output_prob2 = tmpval;
                }
              } else {
                // unphased
                output_prob1 = (cur_dosage * max_output_val + kDosageMid) / kDosageMax;
                output_prob2 = output_prob1;
              }
              if (genodata_alias) {
                genodata_alias[2 * sample_uidx] = output_prob1;
                genodata_alias[2 * sample_uidx + 1] = output_prob2;
              } else {
                bgen_geno_buf_iter[2 * sample_uidx] = output_prob1;
                bgen_geno_buf_iter[2 * sample_uidx + 1] = output_prob2;
              }
            }
          }
        }
        bgen_geno_buf_iter = &(bgen_geno_buf_iter[sample_ct * 2 * (1 + (bit_precision > 8))]);
        /*
      } else if ((!use_phased_format) && (!is_x) && (!cur_y)) {
        // 2 alleles, min ploidy == max ploidy == 1
        // dosages can be patched in after the fact
        *R_CAST(uint32_t*, bgen_geno_buf_iter) = 0x1010002;
        bgen_geno_buf_iter = &(bgen_geno_buf_iter[4]);
        bgen_geno_buf_iter = FillBgen13PloidyAndMissingness(genovec, dosage_present, 1, sample_ct, dosage_ct, bgen_geno_buf_iter);
        *bgen_geno_buf_iter++ = 0;
        *bgen_geno_buf_iter++ = bit_precision;
        const unsigned char* genovec_alias = R_CAST(unsigned char*, genovec);
        if (bit_precision <= 8) {
          uint32_t* genodata_alias = R_CAST(uint32_t*, bgen_geno_buf_iter);
          for (uint32_t geno_byte_idx = 0; geno_byte_idx < sample_ct4; ++geno_byte_idx) {
            const unsigned char cur_geno4 = genovec_alias[geno_byte_idx];
            genodata_alias[geno_byte_idx] = bgen_haploid_hardcall_table8[cur_geno4];
          }
        } else {
          uint64_t* genodata_alias = R_CAST(uint64_t*, bgen_geno_buf_iter);
          for (uint32_t geno_byte_idx = 0; geno_byte_idx < sample_ct4; ++geno_byte_idx) {
            const unsigned char cur_geno4 = genovec_alias[geno_byte_idx];
            genodata_alias[geno_byte_idx] = bgen_haploid_hardcall_table16[cur_geno4];
          }
        }
        if (dosage_ct) {
          uint32_t sample_uidx = 0;
          if (bit_precision <= 8) {
            const uint32_t half_int = kDosageMid - (bit_precision == 1);
            for (uint32_t dosage_idx = 0; dosage_idx < dosage_ct; ++dosage_idx, ++sample_uidx) {
              // multiply by e.g. 255/32768 and round-to-even
              MovU32To1Bit(dosage_present, &sample_uidx);
              // note that this needs to be changed to uint64_t if internal
              // dosage representation is no longer 16-bit
              const uint32_t cur_dosage = dosage_main[dosage_idx];
              const uint32_t output_prob = (cur_dosage * max_output_val + half_int) / kDosageMax;
              bgen_geno_buf_iter[sample_uidx] = output_prob;
            }
          } else {
            uint16_t* genodata_alias = R_CAST(uint16_t*, bgen_geno_buf_iter);
            for (uint32_t dosage_idx = 0; dosage_idx < dosage_ct; ++dosage_idx, ++sample_uidx) {
              MovU32To1Bit(dosage_present, &sample_uidx);
              const uint32_t cur_dosage = dosage_main[dosage_idx];
              const uint32_t output_prob = (cur_dosage * max_output_val + kDosageMid) / kDosageMax;
              genodata_alias[sample_uidx] = output_prob;
            }
          }
        }
        bgen_geno_buf_iter = &(bgen_geno_buf_iter[sample_ct * (1 + (bit_precision > 8))]);
      } else if (!dosage_ct) {
        if (!phasepresent_ct) {
          if (is_x) {
            // if male_ct == 0, we used the all-diploid code path instead
            // if male_ct == sample_ct, we treat as haploid general case
            *R_CAST(uint32_t*, bgen_geno_buf_iter) = 0x2010002;
            bgen_geno_buf_iter = &(bgen_geno_buf_iter[4]);
            unsigned char* sample_ploidy_and_missingness_iter = bgen_geno_buf_iter;
            bgen_geno_buf_iter = &(bgen_geno_buf_iter[sample_ct]);
            *bgen_geno_buf_iter++ = 0;
            *bgen_geno_buf_iter++ = bit_precision;
            uint32_t widx = 0;
            uint32_t loop_len = kBitsPerWordD2;
            if (bit_precision <= 8) {
              while (1) {
                if (widx >= sample_ctl2_m1) {
                  if (widx > sample_ctl2_m1) {
                    break;
                  }
                  loop_len = ModNz(sample_ct, kBitsPerWordD2);
                }
                uintptr_t geno_word = genovec[widx];
                uint32_t male_hw = R_CAST(const Halfword*, sex_male_collapsed)[widx];
                for (uint32_t sample_idx_lowbits = 0; sample_idx_lowbits < loop_len; ++sample_idx_lowbits) {
                  const uintptr_t cur_geno = geno_word & 3;
                  const uint32_t cur_male = male_hw & 1;
                  *sample_ploidy_and_missingness_iter++ = (cur_geno == 3) * 128 + 2 - cur_male;
                  if (cur_male) {
                    *bgen_geno_buf_iter++ = bgen_haploid_basic_table8[cur_geno];
                  } else {
                    *R_CAST(uint16_t*, bgen_geno_buf_iter) = bgen_diploid_basic_table8[cur_geno];
                    bgen_geno_buf_iter = &(bgen_geno_buf_iter[2]);
                  }
                  geno_word >>= 2;
                  male_hw >>= 1;
                }
                ++widx;
              }
            } else {
              uint16_t* genodata_iter = R_CAST(uint16_t*, bgen_geno_buf_iter);
              while (1) {
                if (widx >= sample_ctl2_m1) {
                  if (widx > sample_ctl2_m1) {
                    break;
                  }
                  loop_len = ModNz(sample_ct, kBitsPerWordD2);
                }
                uintptr_t geno_word = genovec[widx];
                uint32_t male_hw = R_CAST(const Halfword*, sex_male_collapsed)[widx];
                for (uint32_t sample_idx_lowbits = 0; sample_idx_lowbits < loop_len; ++sample_idx_lowbits) {
                  const uintptr_t cur_geno = geno_word & 3;
                  const uint32_t cur_male = male_hw & 1;
                  *sample_ploidy_and_missingness_iter++ = (cur_geno == 3) * 128 + 2 - cur_male;
                  if (cur_male) {
                    *genodata_iter++ = bgen_haploid_basic_table16[cur_geno];
                  } else {
                    *R_CAST(uint32_t*, genodata_iter) = bgen_diploid_basic_table16[cur_geno];
                    genodata_iter = &(genodata_iter[2]);
                  }
                  geno_word >>= 2;
                  male_hw >>= 1;
                }
                ++widx;
              }
              bgen_geno_buf_iter = R_CAST(unsigned char*, genodata_iter);
            }
          } else {
            assert(cur_y);
            if ((female_ct != sample_ct) || (!AllBitsAreOne(genovec, sample_ct * 2))) {
              *R_CAST(uint32_t*, bgen_geno_buf_iter) = 0x1000002;
              bgen_geno_buf_iter = &(bgen_geno_buf_iter[4]);
              unsigned char* sample_ploidy_and_missingness_iter = bgen_geno_buf_iter;
              bgen_geno_buf_iter = &(bgen_geno_buf_iter[sample_ct]);
              *bgen_geno_buf_iter++ = 0;
              *bgen_geno_buf_iter++ = bit_precision;
              uint32_t widx = 0;
              uint32_t loop_len = kBitsPerWordD2;
              if (bit_precision <= 8) {
                while (1) {
                  if (widx >= sample_ctl2_m1) {
                    if (widx > sample_ctl2_m1) {
                      break;
                    }
                    loop_len = ModNz(sample_ct, kBitsPerWordD2);
                  }
                  uintptr_t geno_word = genovec[widx];
                  uint32_t female_hw = R_CAST(const Halfword*, sex_female_collapsed)[widx];
                  for (uint32_t sample_idx_lowbits = 0; sample_idx_lowbits < loop_len; ++sample_idx_lowbits) {
                    const uintptr_t cur_geno = geno_word & 3;
                    const uint32_t cur_female = female_hw & 1;
                    const uint32_t ploidy_and_missingness = (cur_geno == 3) * (128 - cur_female) + 1;
                    *sample_ploidy_and_missingness_iter++ = ploidy_and_missingness;
                    if (ploidy_and_missingness != 128) {
                      *bgen_geno_buf_iter++ = bgen_haploid_basic_table8[cur_geno];
                    }
                    geno_word >>= 2;
                    female_hw >>= 1;
                  }
                  ++widx;
                }
              } else {
                uint16_t* genodata_iter = R_CAST(uint16_t*, bgen_geno_buf_iter);
                while (1) {
                  if (widx >= sample_ctl2_m1) {
                    if (widx > sample_ctl2_m1) {
                      break;
                    }
                    loop_len = ModNz(sample_ct, kBitsPerWordD2);
                  }
                  uintptr_t geno_word = genovec[widx];
                  uint32_t female_hw = R_CAST(const Halfword*, sex_female_collapsed)[widx];
                  for (uint32_t sample_idx_lowbits = 0; sample_idx_lowbits < loop_len; ++sample_idx_lowbits) {
                    const uintptr_t cur_geno = geno_word & 3;
                    const uint32_t cur_female = female_hw & 1;
                    const uint32_t ploidy_and_missingness = (cur_geno == 3) * (128 - cur_female) + 1;
                    *sample_ploidy_and_missingness_iter++ = ploidy_and_missingness;
                    if (ploidy_and_missingness != 128) {
                      *genodata_iter++ = bgen_haploid_basic_table16[cur_geno];
                    }
                    geno_word >>= 2;
                    female_hw >>= 1;
                  }
                  ++widx;
                }
                bgen_geno_buf_iter = R_CAST(unsigned char*, genodata_iter);
              }
            } else {
              // chrY, all females, all missing = max ploidy 0
              *R_CAST(uint32_t*, bgen_geno_buf_iter) = 0x0000002;
              bgen_geno_buf_iter = &(bgen_geno_buf_iter[4]);
              bgen_geno_buf_iter = memsetua(bgen_geno_buf_iter, 128, sample_ct);
              *bgen_geno_buf_iter++ = 0;
              *bgen_geno_buf_iter++ = bit_precision;
            }
          }
        } else {
          // hardcall-phase + haploid calls mixed together, ugh
          // no dosages
          if (!is_x) {
            assert(0);
            exit(63);
            // phased calls saved as ploidy 2
            // if chrY, missing female calls saved as ploidy 0
            // everything else is ploidy 1
            //
            // (we previously checked for the pathological all-phased case)
            // uint32_t min_ploidy = 1 - cur_y;
          } else {
            // unphased male calls saved as ploidy 1
            // all other calls saved as ploidy 2
          }
        }
      } else {
        // dosages present, variable ploidy
        // brute force for now
        if (!use_phased_format) {
          if (!is_x) {
            assert(0);
            exit(63);
          } else {
            assert(0);
            exit(63);
          }
        } else {
          if (!is_x) {
            // todo
            assert(0);
            exit(63);
          } else {
            *R_CAST(uint32_t*, bgen_geno_buf_iter) = 0x2010002;
            bgen_geno_buf_iter = &(bgen_geno_buf_iter[4]);
            unsigned char* ploidy_and_missingness_iter = bgen_geno_buf_iter;
            bgen_geno_buf_iter = &(bgen_geno_buf_iter[sample_ct]);
            *bgen_geno_buf_iter++ = 1;
            *bgen_geno_buf_iter++ = bit_precision;
            uint32_t widx = 0;
            uint32_t loop_len = kBitsPerWordD2;
            if (bit_precision <= 8) {
              while (1) {
                if (widx >= sample_ctl2_m1) {
                  if (widx > sample_ctl2_m1) {
                    break;
                  }
                  loop_len = ModNz(sample_ct, kBitsPerWordD2);
                }
                uintptr_t geno_word = genovec[widx];
                uint32_t male_hw = R_CAST(const Halfword*, sex_male_collapsed)[widx];
                ;;;
                for (uint32_t sample_idx_lowbits = 0; sample_idx_lowbits < loop_len; ++sample_idx_lowbits) {
                  const uintptr_t cur_geno = geno_word & 3;
                  const uint32_t cur_male = male_hw & 1;
                  *sample_ploidy_and_missingness_iter++ = (cur_geno == 3) * 128 + 2 - cur_male;
                  if (cur_male) {
                  } else {
                  }
                }
                ++widx;
              }
            } else {
              uint16_t* genodata_iter = R_CAST(uint16_t*, bgen_geno_buf_iter);
              bgen_geno_buf_iter = R_CAST(unsigned char*, genodata_iter);
            }
          }
        }
        */
      }
      const uint32_t uncompressed_bytect = bgen_geno_buf_iter - uncompressed_bgen_geno_buf;
      uintptr_t compressed_bytect;
      if (compressor) {
        compressed_bytect = libdeflate_zlib_compress(compressor, uncompressed_bgen_geno_buf, uncompressed_bytect, writebuf_iter, bgen_compressed_buf_max);
        if (!compressed_bytect) {
          // is this actually possible?
          g_error_ret = kPglRetNomem;
          break;
        }
      } else {
        compressed_bytect = ZSTD_compress(writebuf_iter, bgen_compressed_buf_max, uncompressed_bgen_geno_buf, uncompressed_bytect, zst_level);
        if (ZSTD_isError(compressed_bytect)) {
          // is this actually possible?
          g_error_ret = kPglRetNomem;
          break;
        }
      }
      *variant_bytect_iter++ = 4 + compressed_bytect;
      *variant_bytect_iter++ = uncompressed_bytect;
      writebuf_iter = &(writebuf_iter[bgen_compressed_buf_max]);
      GenovecToMissingnessUnsafe(genovec, sample_ct, missing_acc1);
      if (dosage_ct) {
        BitvecAndNot(dosage_present, sample_ctl, missing_acc1);
      }
      if (is_y) {
        BitvecAnd(sex_male_collapsed, sample_ctl, missing_acc1);
        // ignore missingness state of unknown-sex chrY calls for now
      }
      VcountIncr1To4(missing_acc1, acc1_vec_ct, missing_acc4);
      if (!(--vidx_rem15)) {
        Vcount0Incr4To8(acc4_vec_ct, missing_acc4, missing_acc8);
        vidx_rem15 = 15;
        if (!(--vidx_rem255d15)) {
          Vcount0Incr8To32(acc8_vec_ct, missing_acc8, missing_acc32);
          vidx_rem255d15 = 17;
        }
      }
    }
    if (is_last_block) {
      VcountIncr4To8(missing_acc4, acc4_vec_ct, missing_acc8);
      VcountIncr8To32(missing_acc8, acc8_vec_ct, missing_acc32);
      THREAD_RETURN;
    }
    THREAD_BLOCK_FINISH(tidx);
    parity = 1 - parity;
  }
}

// This allocates exported_sample_ids on top.
BoolErr ExportIdpaste(const uintptr_t* sample_include, const SampleIdInfo* siip, const char* ftypename, uint32_t sample_ct, IdpasteFlags exportf_id_paste, char exportf_id_delim, uintptr_t* max_exported_sample_id_blen_ptr, char** exported_sample_ids_ptr, uint32_t** exported_id_htable_ptr) {
  const uint32_t write_fid = DataFidColIsRequired(sample_include, siip, sample_ct, exportf_id_paste / kfIdpasteMaybefid);
  const char* sample_ids = siip->sample_ids;
  const char* sids = siip->sids;
  const uintptr_t max_sample_id_blen = siip->max_sample_id_blen;
  uintptr_t max_sid_blen = siip->max_sid_blen;
  const uint32_t write_sid = DataSidColIsRequired(sample_include, sids, sample_ct, max_sid_blen, exportf_id_paste / kfIdpasteMaybesid);
  if (write_sid && (!sids)) {
    max_sid_blen = 2;
  }
  uint32_t sample_uidx = 0;
  uint32_t id_delim_warning = 0;
  char id_delim = exportf_id_delim? exportf_id_delim : '_';
  const uintptr_t max_exported_sample_id_blen = max_sample_id_blen + write_sid * max_sid_blen;
  const uint32_t exported_id_htable_size = GetHtableMinSize(sample_ct);
  // check for duplicates
  if (bigstack_alloc_c(sample_ct * max_exported_sample_id_blen, exported_sample_ids_ptr) ||
      bigstack_alloc_u32(exported_id_htable_size, exported_id_htable_ptr)) {
    return 1;
  }
  *max_exported_sample_id_blen_ptr = max_exported_sample_id_blen;
  char* exported_sample_ids = *exported_sample_ids_ptr;
  uint32_t* exported_id_htable = *exported_id_htable_ptr;
  for (uint32_t sample_idx = 0; sample_idx < sample_ct; ++sample_idx, ++sample_uidx) {
    MovU32To1Bit(sample_include, &sample_uidx);
    const char* orig_sample_id = &(sample_ids[sample_uidx * max_sample_id_blen]);
    const char* orig_fid_end = AdvToDelim(orig_sample_id, '\t');
    char* exported_sample_ids_iter = &(exported_sample_ids[sample_idx * max_exported_sample_id_blen]);
    if (write_fid) {
      const uint32_t fid_slen = orig_fid_end - orig_sample_id;
      if ((!id_delim_warning) && memchr(orig_sample_id, id_delim, fid_slen)) {
        id_delim_warning = 1;
      }
      exported_sample_ids_iter = memcpyax(exported_sample_ids_iter, orig_sample_id, fid_slen, id_delim);
    }
    if (exportf_id_paste & kfIdpasteIid) {
      const char* orig_iid = &(orig_fid_end[1]);
      const uint32_t iid_slen = strlen(orig_iid);
      if ((!id_delim_warning) && memchr(orig_iid, id_delim, iid_slen)) {
        id_delim_warning = 1;
      }
      exported_sample_ids_iter = memcpyax(exported_sample_ids_iter, orig_iid, iid_slen, id_delim);
    }
    if (write_sid) {
      if (sids) {
        const char* orig_sid = &(sids[sample_uidx * max_sid_blen]);
        const uint32_t sid_slen = strlen(orig_sid);
        if ((!id_delim_warning) && memchr(orig_sid, id_delim, sid_slen)) {
          id_delim_warning = 1;
        }
        exported_sample_ids_iter = memcpya(exported_sample_ids_iter, orig_sid, sid_slen);
      } else {
        *exported_sample_ids_iter++ = '0';
      }
      ++exported_sample_ids_iter;
    }
    exported_sample_ids_iter[-1] = '\0';
  }
  // todo: revise this warning condition?
  if (id_delim_warning) {
    if (exportf_id_delim) {
      logerrprintfww("Warning: '%c' present in original sample IDs; --export %s will not be able to reconstruct them. Consider rerunning with a different --export id-delim= value.\n", exportf_id_delim, ftypename);
    } else {
      logerrprintfww("Warning: '_' present in original sample IDs; --export %s will not be able to reconstruct them. Consider rerunning with a suitable --export id-delim= value.\n", ftypename);
    }
  }
  if (PopulateStrboxHtable(exported_sample_ids, sample_ct, max_exported_sample_id_blen, exported_id_htable_size, exported_id_htable)) {
    logerrprintfww("Warning: Duplicate sample ID(s) are being written to --export %s file.\n", ftypename);
  }
  return 0;
}

PglErr ExportBgen13(const char* outname, const uintptr_t* sample_include, uint32_t* sample_include_cumulative_popcounts, const SampleIdInfo* siip, const uintptr_t* sex_nm, const uintptr_t* sex_male, const uintptr_t* variant_include, const ChrInfo* cip, const uint32_t* variant_bps, const char* const* variant_ids, const uintptr_t* variant_allele_idxs, const char* const* allele_storage, const AltAlleleCt* refalt1_select, uint32_t sample_ct, uint32_t raw_variant_ct, uint32_t variant_ct, uint32_t max_allele_slen, uint32_t max_thread_ct, ExportfFlags exportf_flags, uint32_t exportf_bits, IdpasteFlags exportf_id_paste, char exportf_id_delim, uintptr_t pgr_alloc_cacheline_ct, PgenFileInfo* pgfip, uint32_t* sample_missing_geno_cts) {
  unsigned char* bigstack_mark = g_bigstack_base;
  ThreadsState ts;
  InitThreads3z(&ts);
  FILE* outfile = nullptr;
  PglErr reterr = kPglRetSuccess;
  {
    // temporarily disable non-diploid
    if (IsHaploidChrPresent(cip)) {
      logerrputs("Error: --export bgen-1.2/1.3 does not support haploid data yet.\n");
      reterr = kPglRetNotYetSupported;
      goto ExportBgen13_ret_1;
    }
    const uint32_t use_zstd_compression = !(exportf_flags & kfExportfBgen12);
    if (use_zstd_compression) {
      g_libdeflate_compressors = nullptr;
    } else {
      g_libdeflate_compressors = S_CAST(struct libdeflate_compressor**, bigstack_alloc(max_thread_ct * sizeof(intptr_t)));
      if (!g_libdeflate_compressors) {
        goto ExportBgen13_ret_NOMEM;
      }
      ZeroPtrArr(max_thread_ct, g_libdeflate_compressors);
    }
    if (exportf_bits > 16) {
      logerrputs("Error: bits= parameter is currently limited to 16.  (This is sufficient to\ncapture all information in a .pgen file.)\n");
      reterr = kPglRetNotYetSupported;
      goto ExportBgen13_ret_1;
    }
    const uint32_t phase_is_present = pgfip->gflags & (kfPgenGlobalHardcallPhasePresent | kfPgenGlobalDosagePhasePresent);
    if (!exportf_bits) {
      // default
      exportf_bits = 16;
    } else if (phase_is_present) {
      if (exportf_bits < 15) {
        if (exportf_bits == 1) {
          logerrputs("Warning: Unphased heterozygous calls in partially-phased variants cannot be\nexported with bits=1.\n");
        } else {
          logerrprintf("Warning: Unphased heterozygous hardcalls in partially-phased variants are\npoorly represented with bits=%u.\n", exportf_bits);
          const double suggested_thresh = 0.00009375 * (16384 >> exportf_bits);
          if (exportf_bits < 4) {
            logerrprintfww("It is necessary to use e.g. --hard-call-threshold %g + --dosage-erase-threshold %g to re-import them cleanly.\n", suggested_thresh, suggested_thresh);
          } else {
            logerrprintfww("It is necessary to use e.g. --dosage-erase-threshold %g to re-import them cleanly.\n", suggested_thresh);
          }
        }
      }
    }
    if (ConstructBgen13LookupTables(exportf_bits)) {
      goto ExportBgen13_ret_NOMEM;
    }
    const uint32_t max_chr_slen = GetMaxChrSlen(cip);
    uintptr_t bgen_geno_cacheline_ct;
    uintptr_t bgen_compressed_buf_max;
    {
      // 4+2+1+1+1+1 + sample_ct * 5 * bytes_per_prob for ordinary diploid case
      const uint64_t bytes_per_prob = DivUp(exportf_bits, 8);
      uint64_t bgen_geno_buf_size = 10 + sample_ct * 5 * bytes_per_prob;
      if (bgen_geno_buf_size > 0xffffffffU - 4) {
        logerrputs("Error: Too many samples for .bgen format.\n");
        goto ExportBgen13_ret_INCONSISTENT_INPUT;
      }
      if (!use_zstd_compression) {
        bgen_compressed_buf_max = libdeflate_deflate_compress_bound(nullptr, bgen_geno_buf_size);
      } else {
        bgen_compressed_buf_max = ZSTD_compressBound(bgen_geno_buf_size);
      }
      // +12 since we may write that far past the end
      bgen_geno_buf_size += 12;
      bgen_geno_cacheline_ct = DivUp(bgen_geno_buf_size, kCacheline);
    }
    g_bgen_compressed_buf_max = bgen_compressed_buf_max;
    // When writing sample ID block, we flush-check after each sample ID.
    // When writing variant data blocks, we flush-check at the beginning of
    // each allele, and at the beginning and end of each genotype data block.
    uintptr_t writebuf_len = 16 + kMaxIdSlen + max_chr_slen;
    if (writebuf_len < max_allele_slen + 4) {
      writebuf_len = max_allele_slen + 4;
    }
    if (writebuf_len < bgen_compressed_buf_max) {
      writebuf_len = bgen_compressed_buf_max;
    }
    writebuf_len += kMaxMediumLine;
    const uint32_t sample_ctl = BitCtToWordCt(sample_ct);
    char* chr_buf;
    unsigned char* writebuf;
    if (bigstack_alloc_c(max_chr_slen, &chr_buf) ||
        bigstack_alloc_uc(writebuf_len, &writebuf) ||
        bigstack_alloc_w(sample_ctl, &g_sex_male_collapsed) ||
        bigstack_alloc_w(sample_ctl, &g_sex_female_collapsed)) {
      goto ExportBgen13_ret_NOMEM;
    }
    CopyBitarrSubset(sex_male, sample_include, sample_ct, g_sex_male_collapsed);
    CopyBitarrSubset(sex_nm, sample_include, sample_ct, g_sex_female_collapsed);
    BitvecAndNot(g_sex_male_collapsed, sample_ctl, g_sex_female_collapsed);

    if (fopen_checked(outname, FOPEN_WB, &outfile)) {
      goto ExportBgen13_ret_OPEN_FAIL;
    }
    // bgen 1.2-1.3 header
    // note that \xxx character constants are interpreted in octal, so \24 is
    // decimal 20, etc.
    unsigned char* write_iter = &(writebuf[4]);
    write_iter = memcpyua(write_iter, "\24\0\0\0", 4);
    write_iter = memcpyua(write_iter, &variant_ct, 4);
    write_iter = memcpyua(write_iter, &sample_ct, 4);
    write_iter = memcpyua(write_iter, "bgen\0\0\0\200", 8);
    // compression mode (1 + use_zstd_compression), layout=2
    writebuf[20] = 9 + use_zstd_compression;
    unsigned char* writebuf_flush = &(writebuf[kMaxMediumLine]);
    // always save sample IDs...
    char* exported_sample_ids;
    uint32_t* exported_id_htable;
    uintptr_t max_exported_sample_id_blen;
    if (ExportIdpaste(sample_include, siip, use_zstd_compression? "bgen-1.3" : "bgen-1.2", sample_ct, exportf_id_paste, exportf_id_delim, &max_exported_sample_id_blen, &exported_sample_ids, &exported_id_htable)) {
      goto ExportBgen13_ret_NOMEM;
    }
    // Compute total length of sample ID block now; this is necessary to fill
    // in bytes 0-3 and 24-27 correctly; and we must do this now since we may
    // flush them before we're done rendering the block.
    uintptr_t sample_id_block_len = 2 * sample_ct + 8;
    const char* exported_sample_ids_iter = exported_sample_ids;
    for (uint32_t sample_idx = 0; sample_idx < sample_ct; ++sample_idx) {
      sample_id_block_len += strlen(exported_sample_ids_iter);
      exported_sample_ids_iter = &(exported_sample_ids_iter[max_exported_sample_id_blen]);
    }
#ifdef __LP64__
    if (sample_id_block_len > 0xffffffffU - 20) {
      // ...unless combined sample ID length is actually greater than 4 GB, in
      // which case bgen-1.2/1.3 can't actually represent it.
      logerrputs("Warning: Omitting sample ID block from .bgen file since it would overflow (more\nthan 4 GB).  Consider using shorter IDs.\n");
      memcpy(writebuf, "\24\0\0\0", 4);
    } else {
#endif
      uint32_t initial_bgen_offset = sample_id_block_len + 20;
      memcpy(writebuf, &initial_bgen_offset, 4);
      *R_CAST(uint32_t*, write_iter) = sample_id_block_len;
      write_iter = &(write_iter[4]);
      write_iter = memcpyua(write_iter, &sample_ct, 4);
      exported_sample_ids_iter = exported_sample_ids;
      for (uint32_t sample_idx = 0; sample_idx < sample_ct; ++sample_idx) {
        const uint16_t cur_slen = strlen(exported_sample_ids_iter);
        write_iter = memcpyua(write_iter, &cur_slen, 2);
        write_iter = memcpyua(write_iter, exported_sample_ids_iter, cur_slen);
        exported_sample_ids_iter = &(exported_sample_ids_iter[max_exported_sample_id_blen]);
        if (fwrite_uflush2(writebuf_flush, outfile, &write_iter)) {
          goto ExportBgen13_ret_WRITE_FAIL;
        }
      }
#ifdef __LP64__
    }
#endif
    BigstackReset(exported_sample_ids);

    const uintptr_t max_write_block_byte_ct = bigstack_left() / 4;
    uint32_t max_write_block_size = kPglVblockSize;
    while (1) {
      // limit each write buffer to 1/4 of remaining workspace
      if ((S_CAST(uint64_t, bgen_compressed_buf_max + 2 * sizeof(int32_t))) * max_write_block_size <= max_write_block_byte_ct) {
        break;
      }
      // 5 bytes per sample * 500k samples = ~2.5 MB per variant; with
      // max_write_block_size lower limit of 256, minimum workspace is ~2.5 GB.
      // That's perfectly reasonable (and it's okay if PgenMtLoadInit raises
      // the requirement to, say, 4 times that).
      // Memory will get tighter once multiallelic variants are supported,
      // though.
      if (max_write_block_size <= kBitsPerVec) {
        goto ExportBgen13_ret_NOMEM;
      }
      max_write_block_size /= 2;
    }

    // todo: test when this saturates
    uint32_t calc_thread_ct = (max_thread_ct > 2)? (max_thread_ct - 1) : max_thread_ct;
    if (calc_thread_ct > max_write_block_size) {
      calc_thread_ct = max_write_block_size;
    }

    if (bigstack_alloc_uc(bgen_compressed_buf_max * max_write_block_size, &(g_writebufs[0])) ||
        bigstack_alloc_uc(bgen_compressed_buf_max * max_write_block_size, &(g_writebufs[1])) ||
        bigstack_alloc_u32(max_write_block_size * 2, &(g_variant_bytects[0])) ||
        bigstack_alloc_u32(max_write_block_size * 2, &(g_variant_bytects[1])) ||
        bigstack_alloc_wp(calc_thread_ct, &g_missing_acc1) ||
        bigstack_alloc_ucp(calc_thread_ct, &g_thread_wkspaces)) {
      goto ExportBgen13_ret_NOMEM;
    }

    const uint32_t acc1_vec_ct = BitCtToVecCt(sample_ct);
    const uint32_t dosage_is_present = pgfip->gflags & kfPgenGlobalDosagePresent;
    const uintptr_t track_missing_cacheline_ct = VecCtToCachelineCt(acc1_vec_ct * 45);
    const uintptr_t thread_xalloc_cacheline_ct = track_missing_cacheline_ct + bgen_geno_cacheline_ct;
    unsigned char* main_loadbufs[2];
    uint32_t read_block_size;
    if (PgenMtLoadInit(variant_include, sample_ct, raw_variant_ct, bigstack_left(), pgr_alloc_cacheline_ct, thread_xalloc_cacheline_ct, 0, pgfip, &calc_thread_ct, &g_genovecs, phase_is_present? (&g_phasepresents) : nullptr, phase_is_present? (&g_phaseinfos) : nullptr, dosage_is_present? (&g_dosage_presents) : nullptr, dosage_is_present? (&g_dosage_mains) : nullptr, phase_is_present? (&g_dphase_presents) : nullptr, phase_is_present? (&g_dphase_deltas) : nullptr, &read_block_size, main_loadbufs, &ts.threads, &g_pgr_ptrs, &g_read_variant_uidx_starts)) {
      goto ExportBgen13_ret_NOMEM;
    }
    if (read_block_size > max_write_block_size) {
      read_block_size = max_write_block_size;
    }

    const uint32_t ref_allele_last = !(exportf_flags & kfExportfRefFirst);
    for (uint32_t tidx = 0; tidx < calc_thread_ct; ++tidx) {
      g_missing_acc1[tidx] = S_CAST(uintptr_t*, bigstack_alloc_raw(track_missing_cacheline_ct * kCacheline));
      g_thread_wkspaces[tidx] = S_CAST(unsigned char*, bigstack_alloc_raw(bgen_geno_cacheline_ct * kCacheline));
    }
    g_sample_ct = sample_ct;
    g_variant_include = variant_include;
    g_sample_include = sample_include;
    g_sample_include_cumulative_popcounts = sample_include_cumulative_popcounts;
    g_calc_thread_ct = calc_thread_ct;
    ts.calc_thread_ct = calc_thread_ct;
    g_refalt1_select = refalt1_select;
    GetXymtStartAndEnd(cip, kChrOffsetY, &g_y_start, &g_y_end);
    g_ref_allele_last = ref_allele_last;
    g_cip = cip;
    g_error_ret = kPglRetSuccess;
    if (!use_zstd_compression) {
      for (uint32_t tidx = 0; tidx < calc_thread_ct; ++tidx) {
        g_libdeflate_compressors[tidx] = libdeflate_alloc_compressor(6);
        if (!g_libdeflate_compressors[tidx]) {
          goto ExportBgen13_ret_NOMEM;
        }
      }
    }

    // Main workflow:
    // 1. Set n=0, load/skip block 0
    //
    // 2. Spawn threads processing block n
    // 3. If n>0, write results for block (n-1)
    // 4. Increment n by 1
    // 5. Load/skip block n unless eof
    // 6. Join threads
    // 7. Goto step 2 unless eof
    //
    // 8. Write results for last block
    const uint32_t read_block_sizel = BitCtToWordCt(read_block_size);
    const uint32_t read_block_ct_m1 = (raw_variant_ct - 1) / read_block_size;
    uint32_t parity = 0;
    uint32_t read_block_idx = 0;
    uint32_t write_variant_uidx = 0;
    uint32_t chr_fo_idx = UINT32_MAX;
    uint32_t chr_end = 0;
    uint32_t chr_slen = 0;

    uint32_t prev_block_write_ct = 0;
    uint32_t variant_idx = 0;
    uint32_t cur_read_block_size = read_block_size;
    uint32_t pct = 0;
    uint32_t next_print_variant_idx = variant_ct / 100;
    logprintfww5("Writing %s ... ", outname);
    fputs("0%", stdout);
    fflush(stdout);
    uint32_t ref_allele_idx = 0;
    uint32_t alt1_allele_idx = 1;
    while (1) {
      uintptr_t cur_block_write_ct = 0;
      if (!ts.is_last_block) {
        while (read_block_idx < read_block_ct_m1) {
          cur_block_write_ct = PopcountWords(&(variant_include[read_block_idx * read_block_sizel]), read_block_sizel);
          if (cur_block_write_ct) {
            break;
          }
          ++read_block_idx;
        }
        if (read_block_idx == read_block_ct_m1) {
          cur_read_block_size = raw_variant_ct - (read_block_idx * read_block_size);
          cur_block_write_ct = PopcountWords(&(variant_include[read_block_idx * read_block_sizel]), BitCtToWordCt(cur_read_block_size));
        }
        if (PgfiMultiread(variant_include, read_block_idx * read_block_size, read_block_idx * read_block_size + cur_read_block_size, cur_block_write_ct, pgfip)) {
          goto ExportBgen13_ret_READ_FAIL;
        }
      }
      if (variant_idx) {
        JoinThreads3z(&ts);
        reterr = g_error_ret;
        if (reterr) {
          if (reterr == kPglRetMalformedInput) {
            logputs("\n");
            logerrputs("Error: Malformed .pgen file.\n");
          }
          goto ExportBgen13_ret_1;
        }
      }
      if (!ts.is_last_block) {
        g_cur_block_write_ct = cur_block_write_ct;
        ComputeUidxStartPartition(variant_include, cur_block_write_ct, calc_thread_ct, read_block_idx * read_block_size, g_read_variant_uidx_starts);
        for (uint32_t tidx = 0; tidx < calc_thread_ct; ++tidx) {
          g_pgr_ptrs[tidx]->fi.block_base = pgfip->block_base;
          g_pgr_ptrs[tidx]->fi.block_offset = pgfip->block_offset;
        }
        ts.is_last_block = (variant_idx + cur_block_write_ct == variant_ct);
        ts.thread_func_ptr = ExportBgen13Thread;
        if (SpawnThreads3z(variant_idx, &ts)) {
          goto ExportBgen13_ret_THREAD_CREATE_FAIL;
        }
      }
      parity = 1 - parity;
      if (variant_idx) {
        // write *previous* block results
        const unsigned char* compressed_data_iter = g_writebufs[parity];
        const uint32_t* variant_bytect_iter = g_variant_bytects[parity];
        for (uint32_t variant_bidx = 0; variant_bidx < prev_block_write_ct; ++variant_bidx, ++write_variant_uidx) {
          MovU32To1Bit(variant_include, &write_variant_uidx);
          if (write_variant_uidx >= chr_end) {
            do {
              ++chr_fo_idx;
              chr_end = cip->chr_fo_vidx_start[chr_fo_idx + 1];
            } while (write_variant_uidx >= chr_end);
            const uint32_t chr_idx = cip->chr_file_order[chr_fo_idx];
            const char* chr_name_end = chrtoa(cip, chr_idx, chr_buf);
            chr_slen = chr_name_end - chr_buf;
          }
          const char* cur_variant_id = variant_ids[write_variant_uidx];
          const uint32_t id_slen = strlen(cur_variant_id);
          // low 16 bits = null "SNP ID"
          *R_CAST(uint32_t*, write_iter) = id_slen << 16;
          write_iter = &(write_iter[4]);
          write_iter = memcpyua(write_iter, cur_variant_id, id_slen);
          *R_CAST(uint16_t*, write_iter) = chr_slen;
          write_iter = &(write_iter[2]);
          write_iter = memcpyua(write_iter, chr_buf, chr_slen);
          write_iter = memcpyua(write_iter, &(variant_bps[write_variant_uidx]), 4);
          uintptr_t variant_allele_idx_base = write_variant_uidx * 2;
          if (variant_allele_idxs) {
            variant_allele_idx_base = variant_allele_idxs[write_variant_uidx];
          }
          const char* const* cur_alleles = &(allele_storage[variant_allele_idx_base]);
          if (refalt1_select) {
            ref_allele_idx = refalt1_select[write_variant_uidx * 2];
            alt1_allele_idx = refalt1_select[write_variant_uidx * 2 + 1];
          }
          write_iter = memcpyua(write_iter, "\2", 2);
          // todo: multiallelic support
          const char* first_allele;
          const char* second_allele;
          if (ref_allele_last) {
            first_allele = cur_alleles[alt1_allele_idx];
            second_allele = cur_alleles[ref_allele_idx];
          } else {
            first_allele = cur_alleles[ref_allele_idx];
            second_allele = cur_alleles[alt1_allele_idx];
          }
          uint32_t allele_slen = strlen(first_allele);
          write_iter = memcpyua(write_iter, &allele_slen, 4);
          if (fwrite_uflush2(writebuf_flush, outfile, &write_iter)) {
            goto ExportBgen13_ret_WRITE_FAIL;
          }
          write_iter = memcpyua(write_iter, first_allele, allele_slen);
          allele_slen = strlen(second_allele);
          write_iter = memcpyua(write_iter, &allele_slen, 4);
          if (fwrite_uflush2(writebuf_flush, outfile, &write_iter)) {
            goto ExportBgen13_ret_WRITE_FAIL;
          }
          write_iter = memcpyua(write_iter, second_allele, allele_slen);
          write_iter = memcpyua(write_iter, variant_bytect_iter, 8);
          const uint32_t cur_compressed_bytect = *variant_bytect_iter - 4;
          variant_bytect_iter = &(variant_bytect_iter[2]);
          if (fwrite_uflush2(writebuf_flush, outfile, &write_iter)) {
            goto ExportBgen13_ret_WRITE_FAIL;
          }
          // may want to elide this memcpy when possible
          write_iter = memcpyua(write_iter, compressed_data_iter, cur_compressed_bytect);
          compressed_data_iter = &(compressed_data_iter[bgen_compressed_buf_max]);
          if (fwrite_uflush2(writebuf_flush, outfile, &write_iter)) {
            goto ExportBgen13_ret_WRITE_FAIL;
          }
        }
      }
      if (variant_idx == variant_ct) {
        break;
      }
      if (variant_idx >= next_print_variant_idx) {
        if (pct > 10) {
          putc_unlocked('\b', stdout);
        }
        pct = (variant_idx * 100LLU) / variant_ct;
        printf("\b\b%u%%", pct++);
        fflush(stdout);
        next_print_variant_idx = (pct * S_CAST(uint64_t, variant_ct)) / 100;
      }
      ++read_block_idx;
      prev_block_write_ct = cur_block_write_ct;
      variant_idx += cur_block_write_ct;
      pgfip->block_base = main_loadbufs[parity];
    }
    if (fclose_uflush_null(writebuf_flush, write_iter, &outfile)) {
      goto ExportBgen13_ret_WRITE_FAIL;
    }
    if (pct > 10) {
      putc_unlocked('\b', stdout);
    }
    fputs("\b\b", stdout);
    logprintf("done.\n");
    const uint32_t sample_ctav = acc1_vec_ct * kBitsPerVec;
    const uintptr_t acc32_offset = acc1_vec_ct * (13 * k1LU * kWordsPerVec);
    uint32_t* scrambled_missing_cts = R_CAST(uint32_t*, &(g_missing_acc1[0][acc32_offset]));
    for (uint32_t tidx = 1; tidx < calc_thread_ct; ++tidx) {
      const uint32_t* thread_scrambled_missing_cts = R_CAST(uint32_t*, &(g_missing_acc1[tidx][acc32_offset]));
      for (uint32_t uii = 0; uii < sample_ctav; ++uii) {
        scrambled_missing_cts[uii] += thread_scrambled_missing_cts[uii];
      }
    }
    for (uint32_t sample_idx = 0; sample_idx < sample_ct; ++sample_idx) {
      const uint32_t scrambled_idx = VcountScramble1(sample_idx);
      sample_missing_geno_cts[sample_idx] = scrambled_missing_cts[scrambled_idx];
    }
  }
  while (0) {
  ExportBgen13_ret_NOMEM:
    reterr = kPglRetNomem;
    break;
  ExportBgen13_ret_OPEN_FAIL:
    reterr = kPglRetOpenFail;
    break;
  ExportBgen13_ret_READ_FAIL:
    reterr = kPglRetReadFail;
    break;
  ExportBgen13_ret_WRITE_FAIL:
    reterr = kPglRetWriteFail;
    break;
  ExportBgen13_ret_INCONSISTENT_INPUT:
    reterr = kPglRetInconsistentInput;
    break;
  ExportBgen13_ret_THREAD_CREATE_FAIL:
    reterr = kPglRetThreadCreateFail;
    break;
  }
 ExportBgen13_ret_1:
  CleanupThreads3z(&ts, &g_cur_block_write_ct);
  if (g_libdeflate_compressors) {
    for (uint32_t tidx = 0; tidx < max_thread_ct; ++tidx) {
      if (!g_libdeflate_compressors[tidx]) {
        break;
      }
      libdeflate_free_compressor(g_libdeflate_compressors[tidx]);
    }
    g_libdeflate_compressors = nullptr;
  }
  fclose_cond(outfile);
  BigstackReset(bigstack_mark);
  return reterr;
}

PglErr ExportOxSample(const char* outname, const uintptr_t* sample_include, const char* sample_ids, const uint32_t* sample_missing_geno_cts, const uintptr_t* sex_nm, const uintptr_t* sex_male, const PhenoCol* pheno_cols, const char* pheno_names, uint32_t sample_ct, uintptr_t max_sample_id_blen, uint32_t pheno_ct, uintptr_t max_pheno_name_blen, uint32_t variant_ct, uint32_t y_ct) {
  unsigned char* bigstack_mark = g_bigstack_base;
  FILE* outfile = nullptr;
  PglErr reterr = kPglRetSuccess;
  {
    const uint32_t pheno_ctl = BitCtToWordCt(pheno_ct);
    char* writebuf;
    uintptr_t* is_basic_categorical;
    // if phenotype is categorical, and all (non-null) category names are of
    // the form P[positive integer], then it's best to emit the positive
    // integer in the name string instead of the internal index.
    if (bigstack_calloc_w(pheno_ctl, &is_basic_categorical) ||
        bigstack_alloc_c(kMaxMediumLine + max_sample_id_blen + 32 + pheno_ct * MAXV(kMaxMissingPhenostrBlen, 16), &writebuf)) {
      goto ExportOxSample_ret_NOMEM;
    }

    if (fopen_checked(outname, FOPEN_WB, &outfile)) {
      goto ExportOxSample_ret_OPEN_FAIL;
    }
    char* writebuf_flush = &(writebuf[kMaxMediumLine]);
    char* write_iter = strcpya(writebuf, "ID_1 ID_2 missing sex");
    for (uint32_t pheno_idx = 0; pheno_idx < pheno_ct; ++pheno_idx) {
      *write_iter++ = ' ';
      write_iter = strcpya(write_iter, &(pheno_names[pheno_idx * max_pheno_name_blen]));
      const PhenoCol* cur_pheno_col = &(pheno_cols[pheno_idx]);
      if (cur_pheno_col->type_code == kPhenoDtypeCat) {
        const uint32_t nn_cat_ct = cur_pheno_col->nonnull_category_ct;
        const char* const* cur_cat_names = cur_pheno_col->category_names;
        uint32_t cat_idx;
        for (cat_idx = 1; cat_idx <= nn_cat_ct; ++cat_idx) {
          const char* cat_name_iter = cur_cat_names[cat_idx];
          if (*cat_name_iter == 'C') {
            uint32_t char_code = *(++cat_name_iter);
            if ((char_code - 49) < 9) {
              uint32_t uii;
              if (!ScanPosintCapped(cat_name_iter, 0x7fffffff, &uii)) {
                continue;
              }
            }
          }
          break;
        }
        if (cat_idx == nn_cat_ct + 1) {
          SetBit(pheno_idx, is_basic_categorical);
        }
      }
      if (fwrite_ck(writebuf_flush, outfile, &write_iter)) {
        goto ExportOxSample_ret_WRITE_FAIL;
      }
    }
    AppendBinaryEoln(&write_iter);

    write_iter = strcpya(write_iter, "0 0 0 D");
    for (uint32_t pheno_idx = 0; pheno_idx < pheno_ct; ++pheno_idx) {
      *write_iter++ = ' ';
      const PhenoDtype cur_type_code = pheno_cols[pheno_idx].type_code;
      if (cur_type_code == kPhenoDtypeCc) {
        *write_iter++ = 'B';
      } else if (cur_type_code == kPhenoDtypeQt) {
        // .psam file does not distinguish between "continuous covariate" and
        // "continuous phenotype", that's lost on round-trip
        *write_iter++ = 'P';
      } else {
        *write_iter++ = 'D';
      }
      if (fwrite_ck(writebuf_flush, outfile, &write_iter)) {
        goto ExportOxSample_ret_WRITE_FAIL;
      }
    }
    AppendBinaryEoln(&write_iter);

    const double nonmale_geno_ct_recip = 1.0 / u31tod(variant_ct - y_ct);
    const double male_geno_ct_recip = 1.0 / u31tod(variant_ct);
    uintptr_t sample_uidx = 0;
    for (uint32_t sample_idx = 0; sample_idx < sample_ct; ++sample_idx, ++sample_uidx) {
      MovWTo1Bit(sample_include, &sample_uidx);
      const char* cur_sample_id = &(sample_ids[max_sample_id_blen * sample_uidx]);
      const char* fid_end = AdvToDelim(cur_sample_id, '\t');
      write_iter = memcpyax(write_iter, cur_sample_id, fid_end - cur_sample_id, ' ');
      write_iter = strcpya(write_iter, &(fid_end[1]));
      *write_iter++ = ' ';
      const int32_t cur_missing_geno_ct = sample_missing_geno_cts[sample_idx];
      if (IsSet(sex_male, sample_uidx)) {
        write_iter = dtoa_g(cur_missing_geno_ct * male_geno_ct_recip, write_iter);
        write_iter = strcpya(write_iter, " 1");
      } else {
        write_iter = dtoa_g(cur_missing_geno_ct * nonmale_geno_ct_recip, write_iter);
        *write_iter++ = ' ';
        if (IsSet(sex_nm, sample_uidx)) {
          *write_iter++ = '2';
        } else {
          write_iter = strcpya(write_iter, "NA");
        }
      }
      for (uint32_t pheno_idx = 0; pheno_idx < pheno_ct; ++pheno_idx) {
        *write_iter++ = ' ';
        const PhenoCol* cur_pheno_col = &(pheno_cols[pheno_idx]);
        if (!IsSet(cur_pheno_col->nonmiss, sample_uidx)) {
          write_iter = strcpya(write_iter, "NA");
        } else {
          const PhenoDtype cur_type_code = cur_pheno_col->type_code;
          if (cur_type_code == kPhenoDtypeCc) {
            *write_iter++ = '0' + IsSet(cur_pheno_col->data.cc, sample_uidx);
          } else if (cur_type_code == kPhenoDtypeQt) {
            write_iter = dtoa_g(cur_pheno_col->data.qt[sample_uidx], write_iter);
          } else {
            const uint32_t cur_cat_idx = cur_pheno_col->data.cat[sample_uidx];
            if (IsSet(is_basic_categorical, pheno_idx)) {
              write_iter = strcpya(write_iter, &(cur_pheno_col->category_names[cur_cat_idx][1]));
            } else {
              write_iter = u32toa(cur_cat_idx, write_iter);
            }
          }
        }
      }
      AppendBinaryEoln(&write_iter);
      if (fwrite_ck(writebuf_flush, outfile, &write_iter)) {
        goto ExportOxSample_ret_WRITE_FAIL;
      }
    }
    if (fclose_flush_null(writebuf_flush, write_iter, &outfile)) {
      goto ExportOxSample_ret_WRITE_FAIL;
    }
  }
  while (0) {
  ExportOxSample_ret_NOMEM:
    reterr = kPglRetNomem;
    break;
  ExportOxSample_ret_OPEN_FAIL:
    reterr = kPglRetOpenFail;
    break;
  ExportOxSample_ret_WRITE_FAIL:
    reterr = kPglRetWriteFail;
    break;
  }
  fclose_cond(outfile);
  BigstackReset(bigstack_mark);
  return reterr;
}

uint32_t ValidVcfAlleleCode(const char* allele_code_iter) {
  // returns 1 if probably valid (angle-bracket case is not exhaustively
  // checked), 0 if definitely not
  uint32_t uii = ctou32(*allele_code_iter);
  if ((uii == '<') || ((uii == '*') && (!allele_code_iter[1]))) {
    return 1;
  }
  do {
    uii -= 64;
    // A = 1, C = 3, G = 7, N = 14, T = 20, so (0x10408a >> ucc) & 1 works as a
    // set membership test
#ifdef __LP64__
    if ((uii > 63) || (!((0x10408a0010408aLLU >> uii) & 1))) {
      // if '[', ']', or '.', assume breakend
      return ((uii == 27) || (uii == 29) || (uii == 0xffffffeeU))? 1 : 0;
    }
#else
    if ((uii > 63) || (!((0x10408a >> (uii % 32)) & 1))) {
      return ((uii == 27) || (uii == 29) || (uii == 0xffffffeeU))? 1 : 0;
    }
#endif
    uii = ctou32(*(++allele_code_iter));
  } while (uii);
  return 1;
}

char* DiploidVcfDosagePrint(uint32_t dosage_int, uint32_t write_ds, char* write_iter) {
  if (write_ds) {
    return PrintSmallDosage(dosage_int, write_iter);
  }
  if (dosage_int <= kDosageMid) {
    write_iter = PrintSmallDosage(kDosageMid - dosage_int, write_iter);
    *write_iter++ = ',';
    write_iter = PrintSmallDosage(dosage_int, write_iter);
    return strcpya(write_iter, ",0");
  }
  write_iter = strcpya(write_iter, "0,");
  write_iter = PrintSmallDosage(kDosageMax - dosage_int, write_iter);
  *write_iter++ = ',';
  return PrintSmallDosage(dosage_int - kDosageMid, write_iter);
}

// assumes rawval is in [0, 327679]
static_assert(kDosageMax == 32768, "HaploidDosagePrint() needs to be updated.");
char* HaploidDosagePrint(uint32_t rawval, char* start) {
  // Instead of constant 5-digit precision, we print fewer digits whenever that
  // doesn't interfere with proper round-tripping.  I.e. we search for the
  // shortest string in
  //   ((n - 0.5)/32768, (n + 0.5)/32768).
  *start++ = '0' + (rawval / 32768);
  rawval = rawval % 32768;
  if (!rawval) {
    // this shouldn't come up for now?
    return start;
  }
  *start++ = '.';

  // (rawval * 2) is in 65536ths
  // 65536 * 625 = 40960k
  const uint32_t range_top_40960k = rawval * 1250 + 625;
  // ok to check half-open interval since we never hit boundary
  if ((range_top_40960k % 4096) < 1250) {
    // when this is true, the four-decimal-place approximation is in the range
    // which round-trips back to our original number.
    const uint32_t four_decimal_places = range_top_40960k / 4096;
    return u32toa_trunc4(four_decimal_places, start);
  }

  // we wish to print (100000 * remainder + 16384) / 32768, left-0-padded.  and
  // may as well banker's round too.
  //
  // banker's rounding yields a different result than regular rounding for n/64
  // when n is congruent to 1 mod 4.  32768/64 = 512.
  const uint32_t five_decimal_places = ((3125 * rawval + 512) / 1024) - ((rawval % 2048) == 512);
  const uint32_t first_decimal_place = five_decimal_places / 10000;
  *start++ = '0' + first_decimal_place;
  const uint32_t last_four_digits = five_decimal_places - first_decimal_place * 10000;
  if (last_four_digits) {
    return u32toa_trunc4(last_four_digits, start);
  }
  return start;
}


#ifdef __arm__
#  error "Unaligned accesses in ExportVcf()."
#endif
PglErr ExportVcf(const uintptr_t* sample_include, const uint32_t* sample_include_cumulative_popcounts, const SampleIdInfo* siip, const uintptr_t* sex_male_collapsed, const uintptr_t* variant_include, const ChrInfo* cip, const uint32_t* variant_bps, const char* const* variant_ids, const uintptr_t* variant_allele_idxs, const char* const* allele_storage, const AltAlleleCt* refalt1_select, const uintptr_t* pvar_qual_present, const float* pvar_quals, const uintptr_t* pvar_filter_present, const uintptr_t* pvar_filter_npass, const char* const* pvar_filter_storage, const char* pvar_info_reload, uintptr_t xheader_blen, InfoFlags info_flags, uint32_t sample_ct, uint32_t raw_variant_ct, uint32_t variant_ct, uint32_t max_allele_slen, uint32_t max_filter_slen, uint32_t info_reload_slen, uint32_t max_thread_ct, ExportfFlags exportf_flags, IdpasteFlags exportf_id_paste, char exportf_id_delim, char* xheader, PgenFileInfo* pgfip, PgenReader* simple_pgrp, char* outname, char* outname_end) {
  unsigned char* bigstack_mark = g_bigstack_base;
  FILE* outfile = nullptr;
  BGZF* bgz_outfile = nullptr;
  PglErr reterr = kPglRetSuccess;
  ReadLineStream pvar_reload_rls;
  PreinitRLstream(&pvar_reload_rls);
  {
    if (!(exportf_flags & kfExportfBgz)) {
      snprintf(outname_end, kMaxOutfnameExtBlen, ".vcf");
      if (fopen_checked(outname, FOPEN_WB, &outfile)) {
        goto ExportVcf_ret_OPEN_FAIL;
      }
    } else {
      snprintf(outname_end, kMaxOutfnameExtBlen, ".vcf.gz");
      bgz_outfile = bgzf_open(outname, "w");
      if (!bgz_outfile) {
        goto ExportVcf_ret_OPEN_FAIL;
      }
#ifndef _WIN32
      if (max_thread_ct > 1) {
        const uint32_t compressor_thread_ct = max_thread_ct - (max_thread_ct > 4);
        if (bgzf_mt(bgz_outfile, MINV(128, compressor_thread_ct), 128)) {
          goto ExportVcf_ret_NOMEM;
        }
      }
#endif
    }
    const uint32_t max_chr_blen = GetMaxChrSlen(cip) + 1;
    // CHROM, POS, ID, REF, one ALT, eoln
    uintptr_t writebuf_blen = kMaxIdSlen + 32 + max_chr_blen + 2 * max_allele_slen;
    // QUAL, FILTER, INFO, FORMAT, genotypes, eoln
    // needs to be larger for >9 alt alleles
    const uint32_t dosage_force = (exportf_flags / kfExportfVcfDosageForce) & 1;
    uint32_t write_ds = (exportf_flags / kfExportfVcfDosageDs) & 1;
    uint32_t write_hds = (exportf_flags / kfExportfVcfDosageHds) & 1;
    uint32_t write_gp_ds_or_hds = write_ds || write_hds || (exportf_flags & kfExportfVcfDosageGp);
    if ((!dosage_force) && write_gp_ds_or_hds && (!(pgfip->gflags & kfPgenGlobalDosagePresent))) {
      write_gp_ds_or_hds = 0;
      logerrprintf("Warning: No dosage data present.  %s field will not be exported.\n", write_hds? "HDS" : (write_ds? "DS" : "GP"));
      write_ds = 0;
      write_hds = 0;
    }
    // GP: 3 limited-precision numbers, up to (7 chars + delim) * 3
    // DS: 1 limited-precision number
    // HDS: 2 limited-precision numbers
    // (could allow e.g. bloated DS+HDS mode, but let's defer that for now)
    const uintptr_t writebuf_blen_lbound = ((4 * k1LU) + write_gp_ds_or_hds * 24 - write_ds * 16 - write_hds * 8) * sample_ct + 32 + max_filter_slen + info_reload_slen;
    if (writebuf_blen < writebuf_blen_lbound) {
      writebuf_blen = writebuf_blen_lbound;
    }
    writebuf_blen += kMaxMediumLine;
    char* writebuf;
    if (bigstack_alloc_c(writebuf_blen, &writebuf)) {
      goto ExportVcf_ret_NOMEM;
    }
    char* writebuf_flush = &(writebuf[kMaxMediumLine]);
    char* write_iter = strcpya(writebuf, "##fileformat=VCFv4.3" EOLN_STR "##fileDate=");
    time_t rawtime;
    time(&rawtime);
    struct tm* loctime;
    loctime = localtime(&rawtime);
    write_iter += strftime(write_iter, kMaxMediumLine, "%Y%m%d", loctime);
    write_iter = strcpya(write_iter, EOLN_STR "##source=PLINKv2.00" EOLN_STR);
    if (cip->chrset_source) {
      AppendChrsetLine(cip, &write_iter);
    }
    if (flexbwrite_flush(writebuf, write_iter - writebuf, outfile, bgz_outfile)) {
      goto ExportVcf_ret_WRITE_FAIL;
    }
    const uint32_t chr_ctl = BitCtToWordCt(cip->chr_ct);
    uintptr_t* written_contig_header_lines;
    if (bigstack_calloc_w(chr_ctl, &written_contig_header_lines)) {
      goto ExportVcf_ret_NOMEM;
    }
    if (xheader) {
      memcpy(writebuf, "##contig=<ID=", 13);
      char* xheader_iter = xheader;
      char* xheader_end = &(xheader[xheader_blen]);
      char* line_end = xheader;
      while (line_end != xheader_end) {
        xheader_iter = line_end;
        line_end = AdvPastDelim(xheader_iter, '\n');
        const uint32_t slen = line_end - xheader_iter;
        if ((slen > 14) && StrStartsWithUnsafe(xheader_iter, "##contig=<ID=")) {
          char* contig_name_start = &(xheader_iter[13]);
          char* contig_name_end = S_CAST(char*, memchr(contig_name_start, ',', slen - 14));
          if (!contig_name_end) {
            // if this line is technically well-formed (ends in '>'), it's
            // useless anyway, throw it out
            continue;
          }
          // if GetChrCodeCounted() is modified to not mutate
          // contig_name_start[], xheader can be changed to const char*
          const uint32_t chr_idx = GetChrCodeCounted(cip, contig_name_end - contig_name_start, contig_name_start);
          if (IsI32Neg(chr_idx)) {
            continue;
          }
          const uint32_t chr_fo_idx = cip->chr_idx_to_foidx[chr_idx];
          if (IsSet(written_contig_header_lines, chr_fo_idx)) {
            logerrputs("Error: Duplicate ##contig line in .pvar file.\n");
            goto ExportVcf_ret_MALFORMED_INPUT;
          }
          SetBit(chr_fo_idx, written_contig_header_lines);
          // if --output-chr was used at some point, we need to sync the
          // ##contig chromosome code with the code in the VCF body.
          write_iter = chrtoa(cip, chr_idx, &(writebuf[13]));
          if (flexbwrite_flush(writebuf, write_iter - writebuf, outfile, bgz_outfile)) {
            goto ExportVcf_ret_WRITE_FAIL;
          }
          if (flexbwrite_flush(contig_name_end, line_end - contig_name_end, outfile, bgz_outfile)) {
            goto ExportVcf_ret_WRITE_FAIL;
          }
        } else {
          if (flexbwrite_flush(xheader_iter, slen, outfile, bgz_outfile)) {
            goto ExportVcf_ret_WRITE_FAIL;
          }
        }
      }
    }
    write_iter = writebuf;
    // fill in the missing ##contig lines
    uint32_t contig_zero_written = 0;
    for (uint32_t chr_fo_idx = 0; chr_fo_idx < cip->chr_ct; ++chr_fo_idx) {
      if (IsSet(written_contig_header_lines, chr_fo_idx)) {
        continue;
      }
      const uint32_t chr_idx = cip->chr_file_order[chr_fo_idx];
      if ((!IsSet(cip->chr_mask, chr_idx)) || AllBitsAreZero(variant_include, cip->chr_fo_vidx_start[chr_fo_idx], cip->chr_fo_vidx_start[chr_fo_idx + 1])) {
        continue;
      }
      char* chr_name_write_start = strcpya(write_iter, "##contig=<ID=");
      char* chr_name_write_end = chrtoa(cip, chr_idx, chr_name_write_start);
      if ((*chr_name_write_start == '0') && (chr_name_write_end == &(chr_name_write_start[1]))) {
        // --allow-extra-chr 0 special case
        if (contig_zero_written) {
          continue;
        }
        contig_zero_written = 1;
        write_iter = strcpya(chr_name_write_end, ",length=2147483645");
      } else {
        if (memchr(chr_name_write_start, ':', chr_name_write_end - chr_name_write_start)) {
          logerrputs("Error: VCF chromosome codes may not include the ':' character.\n");
          goto ExportVcf_ret_MALFORMED_INPUT;
        }
        write_iter = strcpya(chr_name_write_end, ",length=");
        if (1) {
          write_iter = u32toa(variant_bps[cip->chr_fo_vidx_start[chr_fo_idx + 1] - 1] + 1, write_iter);
        } else {
          // todo: unsorted map case
        }
      }
      *write_iter++ = '>';
      AppendBinaryEoln(&write_iter);
      if (flexbwrite_ck(writebuf_flush, outfile, bgz_outfile, &write_iter)) {
        goto ExportVcf_ret_WRITE_FAIL;
      }
    }
    BigstackReset(written_contig_header_lines);
    const uint32_t all_nonref = pgfip->gflags & kfPgenGlobalAllNonref;
    const uintptr_t* nonref_flags = pgfip->nonref_flags;
    const uint32_t raw_variant_ctl = BitCtToWordCt(raw_variant_ct);
    uint32_t write_pr = all_nonref;
    if (nonref_flags) {
      for (uint32_t widx = 0; widx < raw_variant_ctl; ++widx) {
        if (variant_include[widx] & nonref_flags[widx]) {
          write_pr = 1;
          break;
        }
      }
    }
    const uint32_t info_pr_flag_present = (info_flags / kfInfoPrFlagPresent) & 1;
    if (write_pr) {
      if (info_flags & kfInfoPrNonflagPresent) {
        logputs("\n");
        logerrputs("Error: Conflicting INFO:PR fields.  Either fix all REF alleles so that the\n'provisional reference' field is no longer needed, or remove/rename the other\nINFO:PR field.\n");
        goto ExportVcf_ret_INCONSISTENT_INPUT;
      }
      if (!info_pr_flag_present) {
        write_iter = strcpya(write_iter, "##INFO=<ID=PR,Number=0,Type=Flag,Description=\"Provisional reference allele, may not be based on real reference genome\">" EOLN_STR);
      }
    }
    if (write_hds) {
      write_iter = strcpya(write_iter, "##FORMAT=<ID=HDS,Number=2,Type=Float,Description=\"Estimated Haploid Alternate Allele Dosage \">" EOLN_STR);
      logerrputs("Error: VCF HDS output is under development.\n");
      goto ExportVcf_ret_1;
    } else if (write_ds) {
      write_iter = strcpya(write_iter, "##FORMAT=<ID=DS,Number=1,Type=Float,Description=\"Estimated Alternate Allele Dosage : [P(0/1)+2*P(1/1)]\">" EOLN_STR);
    } else if (write_gp_ds_or_hds) {
      write_iter = strcpya(write_iter, "##FORMAT=<ID=GP,Number=G,Type=Float,Description=\"Phred-scaled Genotype Likelihoods\">" EOLN_STR);
    }
    // possible todo: optionally export .psam information as
    // PEDIGREE/META/SAMPLE lines in header, and make --vcf be able to read it
    write_iter = strcpya(write_iter, "##FORMAT=<ID=GT,Number=1,Type=String,Description=\"Genotype\">" EOLN_STR "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT");
    char* exported_sample_ids;
    uint32_t* exported_id_htable;
    uintptr_t max_exported_sample_id_blen;
    if (ExportIdpaste(sample_include, siip, "vcf", sample_ct, exportf_id_paste, exportf_id_delim, &max_exported_sample_id_blen, &exported_sample_ids, &exported_id_htable)) {
      goto ExportVcf_ret_NOMEM;
    }
    for (uint32_t sample_idx = 0; sample_idx < sample_ct; ++sample_idx) {
      *write_iter++ = '\t';
      write_iter = strcpya(write_iter, &(exported_sample_ids[sample_idx * max_exported_sample_id_blen]));
      if (flexbwrite_ck(writebuf_flush, outfile, bgz_outfile, &write_iter)) {
        goto ExportVcf_ret_WRITE_FAIL;
      }
    }
    AppendBinaryEoln(&write_iter);
    BigstackReset(exported_sample_ids);

    logprintfww5("--export vcf%s to %s ... ", bgz_outfile? " bgz" : "", outname);
    fputs("0%", stdout);
    fflush(stdout);

    // includes trailing tab
    char* chr_buf;

    const uint32_t sample_ctl2 = QuaterCtToWordCt(sample_ct);
    const uint32_t sample_ctl = BitCtToWordCt(sample_ct);
    uintptr_t* genovec;
    uintptr_t* allele_include;
    if (bigstack_alloc_c(max_chr_blen, &chr_buf) ||
    // if we weren't using bigstack_alloc, this would need to be sample_ctaw2
        bigstack_alloc_w(sample_ctl2, &genovec) ||
        bigstack_alloc_w(BitCtToWordCt(kPglMaxAltAlleleCt), &allele_include)) {
      goto ExportVcf_ret_NOMEM;
    }
    // For now, if phased data is present, each homozygous call is represented
    // as phased iff the previous heterozygous call was phased.  (If no
    // previous heterozygous call exists, it's treated as phased.)  This does
    // the right thing when the entire genome is phased, and it induces about
    // as good a phase set approximation as you can get without explicitly
    // saving that info.  But that approximation is still pretty inaccurate; as
    // soon as we have any use for them, explicit phase set support should be
    // added to pgenlib.
    const uint32_t some_phased = (pgfip->gflags / kfPgenGlobalHardcallPhasePresent) & 1;
    uintptr_t* prev_phased = nullptr;
    uintptr_t* phasepresent = nullptr;
    uintptr_t* phaseinfo = nullptr;
    if (some_phased) {
      if (bigstack_alloc_w(sample_ctl, &prev_phased) ||
          bigstack_alloc_w(sample_ctl, &phasepresent) ||
          bigstack_alloc_w(sample_ctl, &phaseinfo)) {
        goto ExportVcf_ret_NOMEM;
      }
      SetAllBits(sample_ct, prev_phased);
    }

    uintptr_t* dosage_present = nullptr;
    Dosage* dosage_main = nullptr;
    uintptr_t* dphase_present = nullptr;
    SDosage* dphase_delta = nullptr;
    if (write_gp_ds_or_hds) {
      if (bigstack_alloc_w(sample_ctl, &dosage_present) ||
          bigstack_alloc_dosage(sample_ct, &dosage_main) ||
          bigstack_alloc_w(sample_ctl, &dphase_present) ||
          bigstack_alloc_dphase(sample_ct, &dphase_delta)) {
        goto ExportVcf_ret_NOMEM;
      }
    }

    char* pvar_reload_line_iter = nullptr;
    uint32_t info_col_idx = 0;
    if (pvar_info_reload) {
      reterr = PvarInfoOpenAndReloadHeader(pvar_info_reload, 1 + (max_thread_ct > 1), &pvar_reload_rls, &pvar_reload_line_iter, &info_col_idx);
      if (reterr) {
        goto ExportVcf_ret_1;
      }
    }

    // assumes little-endian
    uint32_t basic_genotext[4];
    basic_genotext[0] = 0x302f3009;  // \t0/0
    basic_genotext[1] = 0x312f3009;  // \t0/1
    basic_genotext[2] = 0x312f3109;  // \t1/1
    basic_genotext[3] = 0x2e2f2e09;  // \t./.
    char haploid_genotext[4][4];
    uint32_t haploid_genotext_blen[8];  // 4..7 = male chrX
    memcpy(haploid_genotext[0], "\t0/0", 4);
    memcpy(haploid_genotext[1], "\t0/1", 4);
    memcpy(haploid_genotext[2], "\t1/1", 4);
    memcpy(haploid_genotext[3], "\t./.", 4);
    haploid_genotext_blen[1] = 4;
    haploid_genotext_blen[4] = 2;
    haploid_genotext_blen[5] = 4;
    haploid_genotext_blen[6] = 2;
    haploid_genotext_blen[7] = 2;
    // don't bother exporting GP for hardcalls
    // usually don't bother for DS, but DS-force is an exception
    char dosage_inttext[16];  // 4..7 = haploid, 5 should never be looked up
    memcpy(dosage_inttext, ":0:1:2:.:0:.:1:.", 16);
    const char* dot_ptr = &(g_one_char_strs[92]);
    const uint32_t sample_ctl2_m1 = sample_ctl2 - 1;
    uint32_t chr_fo_idx = UINT32_MAX;
    uint32_t chr_end = 0;
    uint32_t chr_buf_blen = 0;
    uint32_t variant_uidx = 0;
    uint32_t is_x = 0;
    uint32_t is_haploid = 0;  // includes chrX and chrY
    uint32_t pct = 0;
    uint32_t next_print_variant_idx = variant_ct / 100;
    uint32_t rls_variant_uidx = 0;
    uint32_t ref_allele_idx = 0;
    uint32_t alt1_allele_idx = 1;
    uint32_t cur_allele_ct = 2;
    for (uint32_t variant_idx = 0; variant_idx < variant_ct; ++variant_idx, ++variant_uidx) {
      // a lot of this is redundant with write_pvar(), may want to factor the
      // commonalities out
      MovU32To1Bit(variant_include, &variant_uidx);
      if (variant_uidx >= chr_end) {
        do {
          ++chr_fo_idx;
          chr_end = cip->chr_fo_vidx_start[chr_fo_idx + 1];
        } while (variant_uidx >= chr_end);
        uint32_t chr_idx = cip->chr_file_order[chr_fo_idx];
        is_x = (chr_idx == cip->xymt_codes[kChrOffsetX]);
        is_haploid = IsSet(cip->haploid_mask, chr_idx);
        // forced --merge-par, with diploid male output (is_x NOT set, but
        // chromosome code is X/chrX)
        if ((chr_idx == cip->xymt_codes[kChrOffsetPAR1]) || (chr_idx == cip->xymt_codes[kChrOffsetPAR2])) {
          chr_idx = cip->xymt_codes[kChrOffsetX];
        }
        char* chr_name_end = chrtoa(cip, chr_idx, chr_buf);
        *chr_name_end = '\t';
        chr_buf_blen = 1 + S_CAST(uintptr_t, chr_name_end - chr_buf);
        if (is_haploid) {
          if (is_x) {
            haploid_genotext_blen[0] = 4;
            haploid_genotext_blen[2] = 4;
            haploid_genotext_blen[3] = 4;
          } else {
            haploid_genotext_blen[0] = 2;
            haploid_genotext_blen[2] = 2;
            haploid_genotext_blen[3] = 2;
          }
        }
      }
      // #CHROM
      write_iter = memcpya(write_iter, chr_buf, chr_buf_blen);

      // POS
      write_iter = u32toa_x(variant_bps[variant_uidx], '\t', write_iter);

      // ID
      write_iter = strcpyax(write_iter, variant_ids[variant_uidx], '\t');

      // REF, ALT
      uintptr_t variant_allele_idx_base = variant_uidx * 2;
      if (variant_allele_idxs) {
        variant_allele_idx_base = variant_allele_idxs[variant_uidx];
        cur_allele_ct = variant_allele_idxs[variant_uidx + 1] - variant_allele_idx_base;
      }
      const char* const* cur_alleles = &(allele_storage[variant_allele_idx_base]);
      if (refalt1_select) {
        ref_allele_idx = refalt1_select[variant_uidx * 2];
        alt1_allele_idx = refalt1_select[variant_uidx * 2 + 1];
        // this logic only works in the biallelic case
        assert(cur_allele_ct == 2);
        if (!is_haploid) {
          if (alt1_allele_idx) {
            basic_genotext[0] = 0x302f3009;
            basic_genotext[2] = 0x312f3109;
          } else {
            basic_genotext[0] = 0x312f3109;
            basic_genotext[2] = 0x302f3009;
          }
        } else {
          if (alt1_allele_idx) {
            memcpy(haploid_genotext[0], "\t0/0", 4);
            memcpy(haploid_genotext[2], "\t1/1", 4);
          } else {
            memcpy(haploid_genotext[0], "\t1/1", 4);
            memcpy(haploid_genotext[2], "\t0/0", 4);
          }
        }
        if (alt1_allele_idx) {
          memcpy(dosage_inttext, ":0:1:2:.:0:.:1:.", 16);
        } else {
          memcpy(dosage_inttext, ":2:1:0:.:1:.:0:.", 16);
        }
      }
      if (cur_alleles[ref_allele_idx] != dot_ptr) {
        write_iter = strcpya(write_iter, cur_alleles[ref_allele_idx]);
      } else {
        *write_iter++ = 'N';
      }
      *write_iter++ = '\t';
      write_iter = strcpya(write_iter, cur_alleles[alt1_allele_idx]);
      if (flexbwrite_ck(writebuf_flush, outfile, bgz_outfile, &write_iter)) {
        goto ExportVcf_ret_WRITE_FAIL;
      }
      if (cur_allele_ct > 2) {
        SetAllBits(cur_allele_ct, allele_include);
        ClearBit(ref_allele_idx, allele_include);
        ClearBit(alt1_allele_idx, allele_include);
        uint32_t cur_allele_uidx = 0;
        uint32_t alt_allele_idx = 2;
        do {
          *write_iter++ = ',';
          MovU32To1Bit(allele_include, &cur_allele_uidx);
          write_iter = strcpya(write_iter, cur_alleles[cur_allele_uidx++]);
          if (flexbwrite_ck(writebuf_flush, outfile, bgz_outfile, &write_iter)) {
            goto ExportVcf_ret_WRITE_FAIL;
          }
        } while (++alt_allele_idx < cur_allele_ct);
      }

      // QUAL
      *write_iter++ = '\t';
      if ((!pvar_qual_present) || (!IsSet(pvar_qual_present, variant_uidx))) {
        *write_iter++ = '.';
      } else {
        write_iter = ftoa_g(pvar_quals[variant_uidx], write_iter);
      }

      // FILTER
      *write_iter++ = '\t';
      if ((!pvar_filter_present) || (!IsSet(pvar_filter_present, variant_uidx))) {
        *write_iter++ = '.';
      } else if (!IsSet(pvar_filter_npass, variant_uidx)) {
        write_iter = strcpya(write_iter, "PASS");
      } else {
        write_iter = strcpya(write_iter, pvar_filter_storage[variant_uidx]);
      }

      // INFO
      *write_iter++ = '\t';
      const uint32_t is_pr = all_nonref || (nonref_flags && IsSet(nonref_flags, variant_uidx));
      if (pvar_reload_line_iter) {
        reterr = PvarInfoReloadAndWrite(info_pr_flag_present, info_col_idx, variant_uidx, is_pr, &pvar_reload_rls, &pvar_reload_line_iter, &write_iter, &rls_variant_uidx);
        if (reterr) {
          goto ExportVcf_ret_1;
        }
      } else {
        if (is_pr) {
          write_iter = strcpya(write_iter, "PR");
        } else {
          *write_iter++ = '.';
        }
      }

      // FORMAT
      write_iter = memcpyl3a(write_iter, "\tGT");

      uint32_t dosage_ct = 0;
      uint32_t dphase_ct = 0;
      uint32_t inner_loop_last = kBitsPerWordD2 - 1;
      uint32_t widx = 0;
      if (!some_phased) {
        // biallelic, nothing phased in entire file
        if (!write_gp_ds_or_hds) {
          reterr = PgrGet(sample_include, sample_include_cumulative_popcounts, sample_ct, variant_uidx, simple_pgrp, genovec);
        } else {
          reterr = PgrGetD(sample_include, sample_include_cumulative_popcounts, sample_ct, variant_uidx, simple_pgrp, genovec, dosage_present, dosage_main, &dosage_ct);
        }
        if (reterr) {
          goto ExportVcf_ret_PGR_FAIL;
        }
        if ((!dosage_ct) && (!dosage_force)) {
          if (!is_haploid) {
            // always 4 bytes wide, exploit that
            uint32_t* write_iter_ui_alias = R_CAST(uint32_t*, write_iter);
            while (1) {
              if (widx >= sample_ctl2_m1) {
                if (widx > sample_ctl2_m1) {
                  break;
                }
                inner_loop_last = (sample_ct - 1) % kBitsPerWordD2;
              }
              uintptr_t genovec_word = genovec[widx];
              for (uint32_t sample_idx_lowbits = 0; sample_idx_lowbits <= inner_loop_last; ++sample_idx_lowbits) {
                *write_iter_ui_alias++ = basic_genotext[genovec_word & 3];
                genovec_word >>= 2;
              }
              ++widx;
            }
            write_iter = R_CAST(char*, write_iter_ui_alias);
          } else {
            // chrX: male homozygous/missing calls use only one character + tab
            // other haploid/MT: this is true for nonmales too
            while (1) {
              if (widx >= sample_ctl2_m1) {
                if (widx > sample_ctl2_m1) {
                  break;
                }
                inner_loop_last = (sample_ct - 1) % kBitsPerWordD2;
              }
              uintptr_t genovec_word = genovec[widx];
              uint32_t sex_male_hw = is_x * (R_CAST(const Halfword*, sex_male_collapsed)[widx]);
              for (uint32_t sample_idx_lowbits = 0; sample_idx_lowbits <= inner_loop_last; ++sample_idx_lowbits) {
                const uint32_t cur_geno = genovec_word & 3;
                const uint32_t cur_is_male = sex_male_hw & 1;
                write_iter = memcpya(write_iter, haploid_genotext[cur_geno], haploid_genotext_blen[cur_geno + cur_is_male * 4]);
                genovec_word >>= 2;
                sex_male_hw >>= 1;
              }
              ++widx;
            }
          }
        } else {
          // some dosages present, or DS-force
          if (write_ds) {
            write_iter = memcpyl3a(write_iter, ":DS");
            if (!dosage_ct) {
              // DS-force, need to clear this
              ZeroWArr(sample_ctl, dosage_present);
            }
          } else {
            write_iter = memcpyl3a(write_iter, ":GP");
          }
          if (!alt1_allele_idx) {
            BiallelicDosage16Invert(dosage_ct, dosage_main);
          }
          Dosage* dosage_main_iter = dosage_main;
          if (!is_haploid) {
            while (1) {
              if (widx >= sample_ctl2_m1) {
                if (widx > sample_ctl2_m1) {
                  break;
                }
                inner_loop_last = (sample_ct - 1) % kBitsPerWordD2;
              }
              uintptr_t genovec_word = genovec[widx];
              uint32_t dosage_present_hw = R_CAST(Halfword*, dosage_present)[widx];
              for (uint32_t sample_idx_lowbits = 0; sample_idx_lowbits <= inner_loop_last; ++sample_idx_lowbits) {
                const uint32_t cur_geno = genovec_word & 3;
                write_iter = memcpya(write_iter, &(basic_genotext[cur_geno]), 4);
                if (dosage_present_hw & 1) {
                  *write_iter++ = ':';
                  const uint32_t dosage_int = *dosage_main_iter++;
                  write_iter = DiploidVcfDosagePrint(dosage_int, write_ds, write_iter);
                } else if (dosage_force) {
                  write_iter = memcpya(write_iter, &(dosage_inttext[cur_geno * 2]), 2);
                }
                genovec_word >>= 2;
                dosage_present_hw >>= 1;
              }
              ++widx;
            }
          } else {
            while (1) {
              if (widx >= sample_ctl2_m1) {
                if (widx > sample_ctl2_m1) {
                  break;
                }
                inner_loop_last = (sample_ct - 1) % kBitsPerWordD2;
              }
              uintptr_t genovec_word = genovec[widx];
              uint32_t sex_male_hw = is_x * (R_CAST(const Halfword*, sex_male_collapsed)[widx]);
              uint32_t dosage_present_hw = R_CAST(Halfword*, dosage_present)[widx];
              for (uint32_t sample_idx_lowbits = 0; sample_idx_lowbits <= inner_loop_last; ++sample_idx_lowbits) {
                const uint32_t cur_geno = genovec_word & 3;
                const uint32_t cur_is_male = sex_male_hw & 1;
                const uint32_t cur_genotext_blen = haploid_genotext_blen[cur_geno + cur_is_male * 4];
                write_iter = memcpya(write_iter, haploid_genotext[cur_geno], cur_genotext_blen);
                if (dosage_present_hw & 1) {
                  *write_iter++ = ':';
                  uint32_t dosage_int = *dosage_main_iter++;
                  if (cur_genotext_blen == 2) {
                    if (write_ds) {
                      write_iter = HaploidDosagePrint(dosage_int, write_iter);
                    } else {
                      write_iter = HaploidDosagePrint(kDosageMax - dosage_int, write_iter);
                      *write_iter++ = ',';
                      write_iter = HaploidDosagePrint(dosage_int, write_iter);
                    }
                  } else {
                    // het haploid, or female X
                    write_iter = DiploidVcfDosagePrint(dosage_int, write_ds, write_iter);
                  }
                } else if (dosage_force) {
                  write_iter = memcpya(write_iter, &(dosage_inttext[2 * cur_geno + 16 - 4 * cur_genotext_blen]), 2);
                }
                genovec_word >>= 2;
                sex_male_hw >>= 1;
                dosage_present_hw >>= 1;
              }
              ++widx;
            }
          }
        }
      } else {
        // biallelic, phased
        uint32_t at_least_one_phase_present;
        if (!write_gp_ds_or_hds) {
          reterr = PgrGetP(sample_include, sample_include_cumulative_popcounts, sample_ct, variant_uidx, simple_pgrp, genovec, phasepresent, phaseinfo, &at_least_one_phase_present);
        } else {
          reterr = PgrGetDp(sample_include, sample_include_cumulative_popcounts, sample_ct, variant_uidx, simple_pgrp, genovec, phasepresent, phaseinfo, &at_least_one_phase_present, dosage_present, dosage_main, &dosage_ct, dphase_present, dphase_delta, &dphase_ct);
        }
        if (reterr) {
          goto ExportVcf_ret_PGR_FAIL;
        }
        at_least_one_phase_present = (at_least_one_phase_present != 0);
        if ((!dosage_ct) && (!dosage_force)) {
          if (!is_haploid) {
            uint32_t* write_iter_ui_alias = R_CAST(uint32_t*, write_iter);
            while (1) {
              if (widx >= sample_ctl2_m1) {
                if (widx > sample_ctl2_m1) {
                  break;
                }
                inner_loop_last = (sample_ct - 1) % kBitsPerWordD2;
              }
              uintptr_t genovec_word = genovec[widx];
              uint32_t prev_phased_halfword = R_CAST(Halfword*, prev_phased)[widx];

              // zero this out if phasepresent_ct == 0
              const uint32_t phasepresent_halfword = at_least_one_phase_present * (R_CAST(Halfword*, phasepresent)[widx]);

              const uint32_t phaseinfo_halfword = R_CAST(Halfword*, phaseinfo)[widx];
              for (uint32_t sample_idx_lowbits = 0; sample_idx_lowbits <= inner_loop_last; ++sample_idx_lowbits) {
                const uintptr_t cur_geno = genovec_word & 3;

                // usually "\t0/0", etc.
                uint32_t cur_basic_genotext = basic_genotext[cur_geno];
                if (cur_geno == 1) {
                  const uint32_t cur_shift = (1U << sample_idx_lowbits);
                  if (phasepresent_halfword & cur_shift) {
                    prev_phased_halfword |= cur_shift;
                    if (phaseinfo_halfword & cur_shift) {
                      cur_basic_genotext ^= 0x1000100;  // 0|1 -> 1|0
                    }
                  } else {
                    prev_phased_halfword &= ~cur_shift;
                  }
                }
                // '/' = ascii 47, '|' = ascii 124
                *write_iter_ui_alias++ = cur_basic_genotext + 0x4d0000 * ((prev_phased_halfword >> sample_idx_lowbits) & 1);
                genovec_word >>= 2;
              }
              R_CAST(Halfword*, prev_phased)[widx] = prev_phased_halfword;
              ++widx;
            }
            write_iter = R_CAST(char*, write_iter_ui_alias);
          } else {
            while (1) {
              if (widx >= sample_ctl2_m1) {
                if (widx > sample_ctl2_m1) {
                  break;
                }
                inner_loop_last = (sample_ct - 1) % kBitsPerWordD2;
              }
              uintptr_t genovec_word = genovec[widx];
              uint32_t is_male_hw = is_x * (R_CAST(const Halfword*, sex_male_collapsed)[widx]);
              uint32_t prev_phased_halfword = R_CAST(Halfword*, prev_phased)[widx];

              // zero this out if phasepresent_ct == 0
              const uint32_t phasepresent_halfword = at_least_one_phase_present * (R_CAST(Halfword*, phasepresent)[widx]);

              const uint32_t phaseinfo_halfword = R_CAST(Halfword*, phaseinfo)[widx];
              for (uint32_t sample_idx_lowbits = 0; sample_idx_lowbits <= inner_loop_last; ++sample_idx_lowbits) {
                const uint32_t cur_geno = genovec_word & 3;
                const uint32_t cur_is_male = is_male_hw & 1;
                const uint32_t cur_blen = haploid_genotext_blen[cur_geno + cur_is_male * 4];
                write_iter = memcpya(write_iter, haploid_genotext[cur_geno], cur_blen);
                if (cur_blen == 4) {
                  if (cur_geno == 1) {
                    // a bit redundant with how is_male_hw is handled, but
                    // updating this on every loop iteration doesn't seem better
                    const uint32_t cur_shift = (1U << sample_idx_lowbits);
                    if (phasepresent_halfword & cur_shift) {
                      prev_phased_halfword |= cur_shift;
                      if (phaseinfo_halfword & cur_shift) {
                        memcpy(&(write_iter[-4]), "\t1|0", 4);
                      } else {
                        write_iter[-2] = '|';
                      }
                    } else {
                      prev_phased_halfword &= ~cur_shift;
                    }
                  } else if ((prev_phased_halfword >> sample_idx_lowbits) & 1) {
                    write_iter[-2] = '|';
                  }
                }
                genovec_word >>= 2;
                is_male_hw >>= 1;
              }
              R_CAST(Halfword*, prev_phased)[widx] = prev_phased_halfword;
              ++widx;
            }
          }
        } else {
          // both dosage (or DS-force) and phase present
          if (write_ds) {
            write_iter = memcpyl3a(write_iter, ":DS");
            if (!dosage_ct) {
              ZeroWArr(sample_ctl, dosage_present);
            }
          } else {
            write_iter = memcpyl3a(write_iter, ":GP");
          }
          if (!alt1_allele_idx) {
            BiallelicDosage16Invert(dosage_ct, dosage_main);
          }
          Dosage* dosage_main_iter = dosage_main;
          if (!is_haploid) {
            while (1) {
              if (widx >= sample_ctl2_m1) {
                if (widx > sample_ctl2_m1) {
                  break;
                }
                inner_loop_last = (sample_ct - 1) % kBitsPerWordD2;
              }
              uintptr_t genovec_word = genovec[widx];
              uint32_t prev_phased_halfword = R_CAST(Halfword*, prev_phased)[widx];

              // zero this out if phasepresent_ct == 0
              const uint32_t phasepresent_halfword = at_least_one_phase_present * (R_CAST(Halfword*, phasepresent)[widx]);

              const uint32_t phaseinfo_halfword = R_CAST(Halfword*, phaseinfo)[widx];
              const uint32_t dosage_present_hw = R_CAST(Halfword*, dosage_present)[widx];
              uint32_t cur_shift = 1;
              for (uint32_t sample_idx_lowbits = 0; sample_idx_lowbits <= inner_loop_last; ++sample_idx_lowbits) {
                const uint32_t cur_geno = genovec_word & 3;
                write_iter = memcpya(write_iter, &(basic_genotext[cur_geno]), 4);
                if (cur_geno == 1) {
                  if (phasepresent_halfword & cur_shift) {
                    prev_phased_halfword |= cur_shift;
                    if (phaseinfo_halfword & cur_shift) {
                      memcpy(&(write_iter[-4]), "\t1|0", 4);
                    }
                  } else {
                    prev_phased_halfword &= ~cur_shift;
                  }
                }
                if (prev_phased_halfword & cur_shift) {
                  write_iter[-2] = '|';
                }
                if (dosage_present_hw & cur_shift) {
                  *write_iter++ = ':';
                  const uint32_t dosage_int = *dosage_main_iter++;
                  write_iter = DiploidVcfDosagePrint(dosage_int, write_ds, write_iter);
                } else if (dosage_force) {
                  write_iter = memcpya(write_iter, &(dosage_inttext[cur_geno * 2]), 2);
                }
                genovec_word >>= 2;
                cur_shift <<= 1;
              }
              R_CAST(Halfword*, prev_phased)[widx] = prev_phased_halfword;
              ++widx;
            }
          } else {
            while (1) {
              if (widx >= sample_ctl2_m1) {
                if (widx > sample_ctl2_m1) {
                  break;
                }
                inner_loop_last = (sample_ct - 1) % kBitsPerWordD2;
              }
              uintptr_t genovec_word = genovec[widx];
              uint32_t is_male_hw = is_x * (R_CAST(const Halfword*, sex_male_collapsed)[widx]);
              uint32_t prev_phased_halfword = R_CAST(Halfword*, prev_phased)[widx];

              // zero this out if phasepresent_ct == 0
              const uint32_t phasepresent_halfword = at_least_one_phase_present * (R_CAST(Halfword*, phasepresent)[widx]);

              const uint32_t phaseinfo_halfword = R_CAST(Halfword*, phaseinfo)[widx];
              const uint32_t dosage_present_hw = R_CAST(Halfword*, dosage_present)[widx];
              uint32_t cur_shift = 1;
              for (uint32_t sample_idx_lowbits = 0; sample_idx_lowbits <= inner_loop_last; ++sample_idx_lowbits) {
                const uint32_t cur_geno = genovec_word & 3;
                const uint32_t cur_is_male = is_male_hw & 1;
                const uint32_t cur_blen = haploid_genotext_blen[cur_geno + cur_is_male * 4];
                write_iter = memcpya(write_iter, haploid_genotext[cur_geno], cur_blen);
                if (cur_blen == 4) {
                  if (cur_geno == 1) {
                    if (phasepresent_halfword & cur_shift) {
                      prev_phased_halfword |= cur_shift;
                      if (phaseinfo_halfword & cur_shift) {
                        memcpy(&(write_iter[-4]), "\t1|0", 4);
                      }
                    } else {
                      prev_phased_halfword &= ~cur_shift;
                    }
                  }
                  if (prev_phased_halfword & cur_shift) {
                    write_iter[-2] = '|';
                  }
                  if (dosage_present_hw & cur_shift) {
                    *write_iter++ = ':';
                    const uint32_t dosage_int = *dosage_main_iter++;
                    write_iter = DiploidVcfDosagePrint(dosage_int, write_ds, write_iter);
                  } else if (dosage_force) {
                    write_iter = memcpya(write_iter, &(dosage_inttext[cur_geno * 2]), 2);
                  }
                } else {
                  if (dosage_present_hw & cur_shift) {
                    *write_iter++ = ':';
                    const uint32_t dosage_int = *dosage_main_iter++;
                    if (write_ds) {
                      write_iter = HaploidDosagePrint(dosage_int, write_iter);
                    } else {
                      write_iter = HaploidDosagePrint(kDosageMax - dosage_int, write_iter);
                      *write_iter++ = ',';
                      write_iter = HaploidDosagePrint(dosage_int, write_iter);
                    }
                  } else if (dosage_force) {
                    write_iter = memcpya(write_iter, &(dosage_inttext[cur_geno * 2 + 8]), 2);
                  }
                }
                genovec_word >>= 2;
                is_male_hw >>= 1;
                cur_shift <<= 1;
              }
              R_CAST(Halfword*, prev_phased)[widx] = prev_phased_halfword;
              ++widx;
            }
          }
        }
      }
      // todo: multiallelic cases (separate out cur_allele_ct <= 10)
      AppendBinaryEoln(&write_iter);
      if (flexbwrite_ck(writebuf_flush, outfile, bgz_outfile, &write_iter)) {
        goto ExportVcf_ret_WRITE_FAIL;
      }
      if (variant_idx >= next_print_variant_idx) {
        if (pct > 10) {
          putc_unlocked('\b', stdout);
        }
        pct = (variant_idx * 100LLU) / variant_ct;
        printf("\b\b%u%%", pct++);
        fflush(stdout);
        next_print_variant_idx = (pct * S_CAST(uint64_t, variant_ct)) / 100;
      }
    }
    if (write_iter != writebuf) {
      if (flexbwrite_flush(writebuf, write_iter - writebuf, outfile, bgz_outfile)) {
        goto ExportVcf_ret_WRITE_FAIL;
      }
    }
    if (bgz_outfile) {
      if (bgzf_close(bgz_outfile)) {
        bgz_outfile = nullptr;
        goto ExportVcf_ret_WRITE_FAIL;
      }
      bgz_outfile = nullptr;
    } else {
      if (fclose_null(&outfile)) {
        goto ExportVcf_ret_WRITE_FAIL;
      }
    }
    if (pct > 10) {
      putc_unlocked('\b', stdout);
    }
    fputs("\b\b", stdout);
    logprintf("done.\n");
  }
  while (0) {
  ExportVcf_ret_NOMEM:
    reterr = kPglRetNomem;
    break;
  ExportVcf_ret_OPEN_FAIL:
    reterr = kPglRetOpenFail;
    break;
  ExportVcf_ret_WRITE_FAIL:
    reterr = kPglRetWriteFail;
    break;
  ExportVcf_ret_MALFORMED_INPUT:
    reterr = kPglRetMalformedInput;
    break;
  ExportVcf_ret_INCONSISTENT_INPUT:
    reterr = kPglRetMalformedInput;
    break;
  ExportVcf_ret_PGR_FAIL:
    if (reterr != kPglRetReadFail) {
      logputs("\n");
      logerrputs("Error: Malformed .pgen file.\n");
    }
  }
 ExportVcf_ret_1:
  fclose_cond(outfile);
  CleanupRLstream(&pvar_reload_rls);
  if (bgz_outfile) {
    bgzf_close(bgz_outfile);
  }
  BigstackReset(bigstack_mark);
  return reterr;
}

// more multithread globals
static Dosage* g_smaj_dosagebuf = nullptr;
static uint32_t* g_write_vidx_starts = nullptr;
static const Dosage kGenoToDosage[4] = {0, 16384, 32768, 65535};

THREAD_FUNC_DECL DosageTransposeThread(void* arg) {
  const uintptr_t tidx = R_CAST(uintptr_t, arg);
  const uint32_t sample_ct = g_sample_ct;
  const uint32_t sample_ctd4 = sample_ct / 4;
  const uint32_t sample_rem = sample_ct % 4;
  const uintptr_t sample_ctaw = BitCtToAlignedWordCt(sample_ct);
  const uint32_t sample_ctaw2 = QuaterCtToAlignedWordCt(sample_ct);
  const uint32_t sample_ctab2 = kBytesPerWord * sample_ctaw2;
  const uintptr_t stride = g_stride;
  const uintptr_t* variant_include = g_variant_include;
  const AltAlleleCt* refalt1_select = g_refalt1_select;
  const uintptr_t* sample_include = g_sample_include;
  const uint32_t* sample_include_cumulative_popcounts = g_sample_include_cumulative_popcounts;
  PgenReader* pgrp = g_pgr_ptrs[tidx];
  uintptr_t* genovec_buf = g_thread_write_genovecs[tidx];
  uintptr_t* dosagepresent_buf = g_thread_write_dosagepresents[tidx];
  Dosage* dosagevals_buf = g_thread_write_dosagevals[tidx];
  while (1) {
    const uint32_t is_last_block = g_is_last_thread_block;
    const uint32_t cur_block_write_ct = g_cur_block_write_ct;
    const uint32_t vidx_end = g_write_vidx_starts[tidx + 1];
    uint32_t vidx_start = g_write_vidx_starts[tidx];
    if (cur_block_write_ct && (vidx_end != vidx_start)) {
      uint32_t variant_uidx = g_read_variant_uidx_starts[tidx];
      Dosage* smaj_dosagebuf_iter = &(g_smaj_dosagebuf[vidx_start]);
      uint32_t dosage_cts[kDosagePerCacheline];
      do {
        uint32_t vidx_block_end = RoundDownPow2(vidx_start, kDosagePerCacheline) + kDosagePerCacheline;
        if (vidx_block_end > vidx_end) {
          vidx_block_end = vidx_end;
        }
        const uint32_t vidx_block_size = vidx_block_end - vidx_start;

        // part 1: decompress data
        uintptr_t* genovec_iter = genovec_buf;
        uintptr_t* dosage_present_iter = dosagepresent_buf;
        Dosage* dosage_main_iter = dosagevals_buf;
        for (uint32_t vidx_offset = 0; vidx_offset < vidx_block_size; ++vidx_offset, ++variant_uidx) {
          MovU32To1Bit(variant_include, &variant_uidx);
          uint32_t dosage_ct;
          const PglErr reterr = PgrGetD(sample_include, sample_include_cumulative_popcounts, sample_ct, variant_uidx, pgrp, genovec_iter, dosage_present_iter, R_CAST(uint16_t*, dosage_main_iter), &dosage_ct);
          if (reterr) {
            g_error_ret = reterr;
            goto DosageTransposeThread_fail;
          }
          if ((!refalt1_select) || (!refalt1_select[variant_uidx * 2])) {
            GenovecInvertUnsafe(sample_ct, genovec_iter);
            BiallelicDosage16Invert(dosage_ct, dosage_main_iter);
          }
          genovec_iter = &(genovec_iter[sample_ctaw2]);
          dosage_present_iter = &(dosage_present_iter[sample_ctaw]);
          dosage_main_iter = &(dosage_main_iter[sample_ct]);
          dosage_cts[vidx_offset] = dosage_ct;
        }

        // part 2: process hardcalls for 4 samples at a time
        Dosage* dosagebuf_write_iter0 = smaj_dosagebuf_iter;
        for (uint32_t sample4_idx = 0; sample4_idx < sample_ctd4; ++sample4_idx) {
          Dosage* dosagebuf_write_iter1 = &(dosagebuf_write_iter0[stride]);
          Dosage* dosagebuf_write_iter2 = &(dosagebuf_write_iter1[stride]);
          Dosage* dosagebuf_write_iter3 = &(dosagebuf_write_iter2[stride]);
          const unsigned char* geno_read_iter = &(R_CAST(const unsigned char*, genovec_buf)[sample4_idx]);
          for (uint32_t vidx_offset = 0; vidx_offset < vidx_block_size; ++vidx_offset) {
            uint32_t cur_geno = *geno_read_iter;
            dosagebuf_write_iter0[vidx_offset] = kGenoToDosage[cur_geno & 3];
            dosagebuf_write_iter1[vidx_offset] = kGenoToDosage[(cur_geno >> 2) & 3];
            dosagebuf_write_iter2[vidx_offset] = kGenoToDosage[(cur_geno >> 4) & 3];
            dosagebuf_write_iter3[vidx_offset] = kGenoToDosage[(cur_geno >> 6) & 3];
            geno_read_iter = &(geno_read_iter[sample_ctab2]);
          }
          dosagebuf_write_iter0 = &(dosagebuf_write_iter3[stride]);
        }
        if (sample_rem) {
          const unsigned char* geno_read_iter = &(R_CAST(const unsigned char*, genovec_buf)[sample_ctd4]);
          for (uint32_t vidx_offset = 0; vidx_offset < vidx_block_size; ++vidx_offset) {
            uint32_t cur_geno = *geno_read_iter;
            Dosage* dosagebuf_write_iterx = &(dosagebuf_write_iter0[vidx_offset]);
            uint32_t sample_idx_lowbits = 0;
            while (1) {
              *dosagebuf_write_iterx = kGenoToDosage[cur_geno & 3];
              if (++sample_idx_lowbits == sample_rem) {
                break;
              }
              cur_geno >>= 2;
              dosagebuf_write_iterx = &(dosagebuf_write_iterx[stride]);
            }
            geno_read_iter = &(geno_read_iter[sample_ctab2]);
          }
        }
        // part 3: patch in dosages
        for (uint32_t vidx_offset = 0; vidx_offset < vidx_block_size; ++vidx_offset) {
          const uint32_t cur_dosage_ct = dosage_cts[vidx_offset];
          if (cur_dosage_ct) {
            const uintptr_t* dosage_present = &(dosagepresent_buf[vidx_offset * sample_ctaw]);
            const Dosage* dosage_main = &(dosagevals_buf[vidx_offset * sample_ct]);
            Dosage* cur_dosage_write = &(smaj_dosagebuf_iter[vidx_offset]);
            uint32_t sample_idx = 0;
            for (uint32_t dosage_idx = 0; dosage_idx < cur_dosage_ct; ++dosage_idx, ++sample_idx) {
              MovU32To1Bit(dosage_present, &sample_idx);
              cur_dosage_write[sample_idx * stride] = dosage_main[dosage_idx];
            }
          }
        }
        vidx_start = vidx_block_end;
        smaj_dosagebuf_iter = &(smaj_dosagebuf_iter[vidx_block_size]);
      } while (vidx_start != vidx_end);
    }
  DosageTransposeThread_fail:
    if (is_last_block) {
      THREAD_RETURN;
    }
    THREAD_BLOCK_FINISH(tidx);
  }
}

static_assert(sizeof(Dosage) == 2, "Export012Smaj() needs to be updated.");
PglErr Export012Smaj(const char* outname, const uintptr_t* orig_sample_include, const PedigreeIdInfo* piip, const uintptr_t* sex_nm, const uintptr_t* sex_male, const PhenoCol* pheno_cols, const uintptr_t* variant_include, const char* const* variant_ids, const uintptr_t* variant_allele_idxs, const char* const* allele_storage, const AltAlleleCt* refalt1_select, uint32_t raw_sample_ct, uint32_t sample_ct, uint32_t pheno_ct, uint32_t raw_variant_ct, uint32_t variant_ct, uint32_t max_allele_slen, uint32_t include_dom, uint32_t include_uncounted, uint32_t max_thread_ct, uintptr_t pgr_alloc_cacheline_ct, char exportf_delim, PgenFileInfo* pgfip) {
  unsigned char* bigstack_mark = g_bigstack_base;
  ThreadsState ts;
  InitThreads3z(&ts);
  FILE* outfile = nullptr;
  PglErr reterr = kPglRetSuccess;
  {
    // Write header line; then fully load-and-transpose the first X samples,
    // flush them, load-and-transpose the next X, etc.
    // Similar to ExportIndMajorBed() and Export012Vmaj().
    // Priority is making the with-dosage case work well, since plink 1.9
    // already handles the no-dosage case.
    // (possible todo: have a separate no-dosage fast path)
    if (variant_ct * (1 + include_dom) > (kMaxLongLine - 4 * kMaxIdSlen - 64) / 8) {
      snprintf(g_logbuf, kLogbufSize, "Error: Too many variants for --export A%s.  (Try to work with A-transpose\ninstead.)\n", include_dom? "D" : "");
      goto Export012Smaj_ret_INCONSISTENT_INPUT_2;
    }
    if (fopen_checked(outname, FOPEN_WB, &outfile)) {
      goto Export012Smaj_ret_OPEN_FAIL;
    }
    char* writebuf = g_textbuf;
    if (max_allele_slen > kMaxMediumLine - 5) {
      if (bigstack_alloc_c(kMaxMediumLine + 5 + max_allele_slen, &writebuf)) {
        goto Export012Smaj_ret_NOMEM;
      }
    }
    char* write_iter = writebuf;
    char* writebuf_flush = &(writebuf[kMaxMediumLine]);
    write_iter = memcpyl3a(write_iter, "FID");
    *write_iter++ = exportf_delim;
    write_iter = memcpyl3a(write_iter, "IID");
    *write_iter++ = exportf_delim;
    write_iter = memcpyl3a(write_iter, "PAT");
    *write_iter++ = exportf_delim;
    write_iter = memcpyl3a(write_iter, "MAT");
    *write_iter++ = exportf_delim;
    write_iter = memcpyl3a(write_iter, "SEX");
    *write_iter++ = exportf_delim;
    write_iter = strcpya(write_iter, "PHENOTYPE");
    uint32_t ref_allele_idx = 0;
    uint32_t variant_uidx = 0;
    uint64_t bytes_written = 0;
    for (uint32_t variant_idx = 0; variant_idx < variant_ct; ++variant_idx, ++variant_uidx) {
      MovU32To1Bit(variant_include, &variant_uidx);
      *write_iter++ = exportf_delim;
      const char* cur_var_id = variant_ids[variant_uidx];
      const uint32_t cur_slen = strlen(cur_var_id);
      write_iter = memcpyax(write_iter, cur_var_id, cur_slen, '_');
      uintptr_t variant_allele_idx_base = variant_uidx * 2;
      if (variant_allele_idxs) {
        variant_allele_idx_base = variant_allele_idxs[variant_uidx];
      }
      if (refalt1_select) {
        ref_allele_idx = refalt1_select[2 * variant_uidx];
      }
      write_iter = strcpya(write_iter, allele_storage[variant_allele_idx_base + ref_allele_idx]);
      if (write_iter >= writebuf_flush) {
        bytes_written += write_iter - writebuf;
        if (fwrite_flush2(writebuf_flush, outfile, &write_iter)) {
          goto Export012Smaj_ret_WRITE_FAIL;
        }
      }
      if (include_uncounted) {
        write_iter = strcpya(write_iter, "(/");
        // todo: multiallelic case
        write_iter = strcpya(write_iter, allele_storage[variant_allele_idx_base + 1 - ref_allele_idx]);
        *write_iter++ = ')';
        if (write_iter >= writebuf_flush) {
          bytes_written += write_iter - writebuf;
          if (fwrite_flush2(writebuf_flush, outfile, &write_iter)) {
            goto Export012Smaj_ret_WRITE_FAIL;
          }
        }
      }
      if (include_dom) {
        *write_iter++ = exportf_delim;
        write_iter = memcpya(write_iter, cur_var_id, cur_slen);
        write_iter = strcpya(write_iter, "_HET");
        if (write_iter >= writebuf_flush) {
          bytes_written += write_iter - writebuf;
          if (fwrite_flush2(writebuf_flush, outfile, &write_iter)) {
            goto Export012Smaj_ret_WRITE_FAIL;
          }
        }
      }
    }
    AppendBinaryEoln(&write_iter);
    bytes_written += write_iter - writebuf;
    if (bytes_written > kMaxLongLine) {
      snprintf(g_logbuf, kLogbufSize, "Error: --export A%s header line too long (>2GB).\n", include_dom? "D" : "");
      goto Export012Smaj_ret_INCONSISTENT_INPUT_2;
    }

    uintptr_t* pheno_nm = nullptr;
    uintptr_t* pheno_cc = nullptr;
    double* pheno_qt = nullptr;
    // .raw files don't support categorical phenotypes
    const uint32_t pheno_idx = FirstCcOrQtPhenoIdx(pheno_cols, pheno_ct);
    if (pheno_idx != UINT32_MAX) {
      const PhenoDtype type_code = pheno_cols[pheno_idx].type_code;
      pheno_nm = pheno_cols[pheno_idx].nonmiss;
      if (type_code == kPhenoDtypeCc) {
        pheno_cc = pheno_cols[pheno_idx].data.cc;
      } else {
        pheno_qt = pheno_cols[pheno_idx].data.qt;
      }
    }
    const char* legacy_output_missing_pheno = g_legacy_output_missing_pheno;
    const uint32_t lomp_slen = strlen(legacy_output_missing_pheno);

    // Initially had the main read thread also perform decompression and
    // inversion, and the worker threads only unpacked/transposed the data to a
    // sample-major dosage matrix; but this made it pointless to have more than
    // 1 worker thread.
    //
    // So, moving closer to ExportIndMajorBed() strategy:
    // * Main read thread only loads raw bytes with PgfiMultiread().
    // * Worker thread(s) select subsets of the loaded variants to process in a
    //   cacheline-aware manner: first worker's interval ends with variant_idx
    //   divisible by (kCacheline / sizeof(Dosage)) == 32 (unless it's the
    //   only active worker), and all other interval(s) start with variant_idx
    //   divisible by 32.

    // todo: check when this saturates
    uint32_t calc_thread_ct = (max_thread_ct > 2)? (max_thread_ct - 1) : max_thread_ct;
    if (calc_thread_ct * kDosagePerCacheline > variant_ct) {
      calc_thread_ct = DivUp(variant_ct, kDosagePerCacheline);
    }
    unsigned char* main_loadbufs[2];
    uint32_t read_block_size;

    // note that we only allow this to use 1/4 of remaining memory
    if (PgenMtLoadInit(variant_include, raw_sample_ct, variant_ct, bigstack_left() / 4, pgr_alloc_cacheline_ct, 0, 0, pgfip, &calc_thread_ct, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &read_block_size, main_loadbufs, &ts.threads, &g_pgr_ptrs, &g_read_variant_uidx_starts)) {
      goto Export012Smaj_ret_NOMEM;
    }

    const uint32_t raw_sample_ctl = BitCtToWordCt(raw_sample_ct);
    uintptr_t* sample_include;
    uint32_t* sample_include_cumulative_popcounts;
    if (bigstack_alloc_w(raw_sample_ctl, &sample_include) ||
        bigstack_alloc_u32(raw_sample_ctl, &sample_include_cumulative_popcounts) ||
        bigstack_alloc_u32(calc_thread_ct + 1, &g_write_vidx_starts) ||
        bigstack_alloc_wp(calc_thread_ct, &g_thread_write_genovecs) ||
        bigstack_alloc_wp(calc_thread_ct, &g_thread_write_dosagepresents) ||
        bigstack_alloc_dosagep(calc_thread_ct, &g_thread_write_dosagevals)) {
      goto Export012Smaj_ret_NOMEM;
    }

    // Remaining memory byte requirements:
    //   calc_thread_ct * kDosagePerCacheline * kBytesPerWord *
    //     read_sample_ctaw2 for per-thread genovecs buffers
    //   calc_thread_ct * kDosagePerCacheline * kBytesPerWord *
    //     read_sample_ctaw for per-thread dosage_presents buffers
    //   calc_thread_ct * kDosagePerCacheline * read_sample_ct *
    //     sizeof(Dosage) for per-thread dosage_main buffers
    //   (dosage_ct buffers just go on the thread stacks)
    //   read_sample_ct * variant_ct * sizeof(Dosage) for g_smaj_dosagebuf
    // This is about
    //   read_sample_ct *
    //     (calc_thread_ct * kDosagePerCacheline * 2.375 + variant_ct * 2)
    ts.calc_thread_ct = calc_thread_ct;
    uintptr_t bytes_avail = bigstack_left();
    // account for rounding
    if (bytes_avail < kCacheline + calc_thread_ct * kDosagePerCacheline * (2 * kCacheline)) {
      goto Export012Smaj_ret_NOMEM;
    }
    bytes_avail -= kCacheline + calc_thread_ct * kDosagePerCacheline * (2 * kCacheline);
    uint32_t read_sample_ct = sample_ct;
    uint32_t pass_ct = 1;
    const uintptr_t bytes_per_sample = calc_thread_ct * (kDosagePerCacheline / 8) * (3LLU + 8 * sizeof(Dosage)) + variant_ct * sizeof(Dosage);
    if ((sample_ct * S_CAST(uint64_t, bytes_per_sample)) > bytes_avail) {
      read_sample_ct = bytes_avail / bytes_per_sample;
      if (!read_sample_ct) {
        goto Export012Smaj_ret_NOMEM;
      }
      if (read_sample_ct > 4) {
        read_sample_ct = RoundDownPow2(read_sample_ct, 4);
      }
      pass_ct = 1 + (sample_ct - 1) / read_sample_ct;
    }
    uintptr_t read_sample_ctaw = BitCtToAlignedWordCt(read_sample_ct);
    uintptr_t read_sample_ctaw2 = QuaterCtToAlignedWordCt(read_sample_ct);
    for (uint32_t tidx = 0; tidx < calc_thread_ct; ++tidx) {
      g_thread_write_genovecs[tidx] = S_CAST(uintptr_t*, bigstack_alloc_raw(kDosagePerCacheline * sizeof(intptr_t) * read_sample_ctaw2));
      g_thread_write_dosagepresents[tidx] = S_CAST(uintptr_t*, bigstack_alloc_raw(kDosagePerCacheline * sizeof(intptr_t) * read_sample_ctaw));
      g_thread_write_dosagevals[tidx] = S_CAST(Dosage*, bigstack_alloc_raw(kDosagePerCacheline * sizeof(Dosage) * read_sample_ct));
    }
    g_variant_include = variant_include;
    g_refalt1_select = refalt1_select;
    g_calc_thread_ct = calc_thread_ct;
    g_sample_ct = read_sample_ct;
    g_stride = RoundUpPow2(variant_ct, kDosagePerCacheline);
    g_smaj_dosagebuf = S_CAST(Dosage*, bigstack_alloc_raw_rd(read_sample_ct * S_CAST(uintptr_t, g_stride) * sizeof(Dosage)));
    g_error_ret = kPglRetSuccess;

    const char* sample_ids = piip->sii.sample_ids;
    const char* paternal_ids = piip->parental_id_info.paternal_ids;
    const char* maternal_ids = piip->parental_id_info.maternal_ids;
    const uintptr_t max_sample_id_blen = piip->sii.max_sample_id_blen;
    const uintptr_t max_paternal_id_blen = piip->parental_id_info.max_paternal_id_blen;
    const uintptr_t max_maternal_id_blen = piip->parental_id_info.max_maternal_id_blen;
    const uint32_t read_block_sizel = BitCtToWordCt(read_block_size);
    const uint32_t read_block_ct_m1 = (raw_variant_ct - 1) / read_block_size;
    uint32_t sample_uidx_start = AdvTo1Bit(orig_sample_include, 0);
    for (uint32_t pass_idx = 0; pass_idx < pass_ct; ++pass_idx) {
      memcpy(sample_include, orig_sample_include, raw_sample_ctl * sizeof(intptr_t));
      if (sample_uidx_start) {
        ClearBitsNz(0, sample_uidx_start, sample_include);
      }
      uint32_t sample_uidx_end;
      if (pass_idx + 1 == pass_ct) {
        read_sample_ct = sample_ct - pass_idx * read_sample_ct;
        g_sample_ct = read_sample_ct;
        sample_uidx_end = raw_sample_ct;
        read_sample_ctaw = BitCtToAlignedWordCt(read_sample_ct);
        read_sample_ctaw2 = QuaterCtToAlignedWordCt(read_sample_ct);
      } else {
        sample_uidx_end = FindNth1BitFrom(orig_sample_include, sample_uidx_start + 1, read_sample_ct);
        ClearBitsNz(sample_uidx_end, raw_sample_ct, sample_include);
      }
      FillCumulativePopcounts(sample_include, raw_sample_ctl, sample_include_cumulative_popcounts);
      g_sample_include = sample_include;
      g_sample_include_cumulative_popcounts = sample_include_cumulative_popcounts;
      if (pass_idx) {
        ReinitThreads3z(&ts);
        pgfip->block_base = main_loadbufs[0];
        for (uint32_t tidx = 0; tidx < calc_thread_ct; ++tidx) {
          PgrClearLdCache(g_pgr_ptrs[tidx]);
          g_pgr_ptrs[tidx]->fi.block_base = main_loadbufs[0];
          g_pgr_ptrs[tidx]->fi.block_offset = 0;
        }
      }
      putc_unlocked('\r', stdout);
      printf("--export A%s pass %u/%u: loading... 0%%", include_dom? "D" : "", pass_idx + 1, pass_ct);
      fflush(stdout);
      // Main workflow:
      // 1. Set n=0, load first calc_thread_ct * kDosagePerCacheline
      //    post-filtering variants
      //
      // 2. Spawn threads processing batch n
      // 3. Load batch (n+1) unless eof
      // 4. Join threads
      // 5. Increment n by 1
      // 6. Goto step 2 unless eof
      uint32_t parity = 0;
      uint32_t read_block_idx = 0;
      uint32_t variant_idx = 0;
      uint32_t cur_read_block_size = read_block_size;
      uint32_t pct = 0;
      uint32_t next_print_idx = variant_ct / 100;
      while (1) {
        uintptr_t cur_block_write_ct = 0;
        if (!ts.is_last_block) {
          while (read_block_idx < read_block_ct_m1) {
            cur_block_write_ct = PopcountWords(&(variant_include[read_block_idx * read_block_sizel]), read_block_sizel);
            if (cur_block_write_ct) {
              break;
            }
            ++read_block_idx;
          }
          if (read_block_idx == read_block_ct_m1) {
            cur_read_block_size = raw_variant_ct - (read_block_idx * read_block_size);
            cur_block_write_ct = PopcountWords(&(variant_include[read_block_idx * read_block_sizel]), BitCtToWordCt(cur_read_block_size));
          }
          if (PgfiMultiread(variant_include, read_block_idx * read_block_size, read_block_idx * read_block_size + cur_read_block_size, cur_block_write_ct, pgfip)) {
            goto Export012Smaj_ret_THREAD_CREATE_FAIL;
          }
        }
        if (variant_idx) {
          JoinThreads3z(&ts);
          reterr = g_error_ret;
          if (reterr) {
            if (reterr == kPglRetMalformedInput) {
              logputs("\n");
              logerrputs("Error: Malformed .pgen file.\n");
            }
            goto Export012Smaj_ret_1;
          }
        }
        if (!ts.is_last_block) {
          g_cur_block_write_ct = cur_block_write_ct;
          ComputePartitionAligned(variant_include, calc_thread_ct, read_block_idx * read_block_size, variant_idx, cur_block_write_ct, kDosagePerCacheline, g_read_variant_uidx_starts, g_write_vidx_starts);
          for (uint32_t tidx = 0; tidx < calc_thread_ct; ++tidx) {
            g_pgr_ptrs[tidx]->fi.block_base = pgfip->block_base;
            g_pgr_ptrs[tidx]->fi.block_offset = pgfip->block_offset;
          }
          ts.is_last_block = (variant_idx + cur_block_write_ct == variant_ct);
          ts.thread_func_ptr = DosageTransposeThread;
          if (SpawnThreads3z(read_block_idx, &ts)) {
            goto Export012Smaj_ret_THREAD_CREATE_FAIL;
          }
        }
        parity = 1 - parity;
        if (variant_idx == variant_ct) {
          break;
        }
        if (variant_idx >= next_print_idx) {
          if (pct > 10) {
            putc_unlocked('\b', stdout);
          }
          pct = (variant_idx * 100LLU) / variant_ct;
          printf("\b\b%u%%", pct++);
          fflush(stdout);
          next_print_idx = (pct * S_CAST(uint64_t, variant_ct)) / 100;
        }

        ++read_block_idx;
        variant_idx += cur_block_write_ct;
        pgfip->block_base = main_loadbufs[parity];
      }
      if (pct > 10) {
        fputs("\b \b", stdout);
      }
      fputs("\b\b\b\b\b\b\b\b\b\b\b\b\bwriting... 0%", stdout);
      fflush(stdout);
      pct = 0;
      next_print_idx = read_sample_ct / 100;
      uint32_t sample_uidx = sample_uidx_start;
      const Dosage* cur_dosage_row = g_smaj_dosagebuf;
      for (uint32_t sample_idx = 0; sample_idx < read_sample_ct; ++sample_idx, ++sample_uidx) {
        MovU32To1Bit(sample_include, &sample_uidx);
        const char* cur_sample_fid = &(sample_ids[sample_uidx * max_sample_id_blen]);
        const char* fid_end = AdvToDelim(cur_sample_fid, '\t');
        write_iter = memcpyax(write_iter, cur_sample_fid, fid_end - cur_sample_fid, exportf_delim);
        write_iter = strcpyax(write_iter, &(fid_end[1]), exportf_delim);
        write_iter = strcpyax(write_iter, &(paternal_ids[sample_uidx * max_paternal_id_blen]), exportf_delim);
        write_iter = strcpyax(write_iter, &(maternal_ids[sample_uidx * max_maternal_id_blen]), exportf_delim);
        *write_iter++ = Sexchar(sex_nm, sex_male, sample_uidx);
        *write_iter++ = exportf_delim;
        if ((!pheno_nm) || (!IsSet(pheno_nm, sample_uidx))) {
          write_iter = memcpya(write_iter, legacy_output_missing_pheno, lomp_slen);
        } else if (pheno_cc) {
          *write_iter++ = '1' + IsSet(pheno_cc, sample_uidx);
        } else {
          write_iter = dtoa_g(pheno_qt[sample_uidx], write_iter);
        }
        if (fwrite_ck(writebuf_flush, outfile, &write_iter)) {
          goto Export012Smaj_ret_WRITE_FAIL;
        }
        for (variant_idx = 0; variant_idx < variant_ct; ++variant_idx) {
          *write_iter++ = exportf_delim;
          uint32_t cur_dosage_val = cur_dosage_row[variant_idx];
          if (cur_dosage_val != 65535) {
            write_iter = PrintSmallDosage(cur_dosage_val, write_iter);
            if (include_dom) {
              *write_iter++ = exportf_delim;
              write_iter = PrintSmallDosage(16384 - abs_i32(cur_dosage_val - 16384), write_iter);
            }
          } else {
            write_iter = strcpya(write_iter, "NA");
            if (include_dom) {
              *write_iter++ = exportf_delim;
              write_iter = strcpya(write_iter, "NA");
            }
          }
          // todo: try making this check less frequently
          if (fwrite_ck(writebuf_flush, outfile, &write_iter)) {
            goto Export012Smaj_ret_WRITE_FAIL;
          }
        }
        AppendBinaryEoln(&write_iter);
        cur_dosage_row = &(cur_dosage_row[g_stride]);
        if (sample_idx >= next_print_idx) {
          if (pct > 10) {
            putc_unlocked('\b', stdout);
          }
          pct = (sample_idx * 100LLU) / read_sample_ct;
          printf("\b\b%u%%", pct++);
          fflush(stdout);
          next_print_idx = (pct * S_CAST(uint64_t, read_sample_ct)) / 100;
        }
      }
      sample_uidx_start = sample_uidx_end;
      if (pct > 10) {
        fputs("\b \b", stdout);
      }
    }
    if (fclose_flush_null(writebuf_flush, write_iter, &outfile)) {
      goto Export012Smaj_ret_WRITE_FAIL;
    }
    fputs("\b\bdone.\n", stdout);
    logprintfww("--export A%s: %s written.\n", include_dom? "D" : "", outname);
  }
  while (0) {
  Export012Smaj_ret_NOMEM:
    reterr = kPglRetNomem;
    break;
  Export012Smaj_ret_OPEN_FAIL:
    reterr = kPglRetOpenFail;
    break;
  Export012Smaj_ret_WRITE_FAIL:
    reterr = kPglRetWriteFail;
    break;
  Export012Smaj_ret_INCONSISTENT_INPUT_2:
    logerrputsb();
    reterr = kPglRetInconsistentInput;
    break;
  Export012Smaj_ret_THREAD_CREATE_FAIL:
    reterr = kPglRetThreadCreateFail;
    break;
  }
 Export012Smaj_ret_1:
  CleanupThreads3z(&ts, &g_cur_block_write_ct);
  fclose_cond(outfile);
  pgfip->block_base = nullptr;
  BigstackReset(bigstack_mark);
  return reterr;
}

PglErr Exportf(const uintptr_t* sample_include, const PedigreeIdInfo* piip, const uintptr_t* sex_nm, const uintptr_t* sex_male, const PhenoCol* pheno_cols, const char* pheno_names, const uintptr_t* variant_include, const ChrInfo* cip, const uint32_t* variant_bps, const char* const* variant_ids, const uintptr_t* variant_allele_idxs, const char* const* allele_storage, const AltAlleleCt* refalt1_select, const uintptr_t* pvar_qual_present, const float* pvar_quals, const uintptr_t* pvar_filter_present, const uintptr_t* pvar_filter_npass, const char* const* pvar_filter_storage, const char* pvar_info_reload, const double* variant_cms, uintptr_t xheader_blen, InfoFlags info_flags, uint32_t raw_sample_ct, uint32_t sample_ct, uint32_t pheno_ct, uintptr_t max_pheno_name_blen, uint32_t raw_variant_ct, uint32_t variant_ct, uint32_t max_allele_slen, uint32_t max_filter_slen, uint32_t info_reload_slen, uint32_t max_thread_ct, MakePlink2Flags make_plink2_flags, ExportfFlags exportf_flags, IdpasteFlags exportf_id_paste, char exportf_id_delim, __maybe_unused uint32_t exportf_bits, uintptr_t pgr_alloc_cacheline_ct, char* xheader, PgenFileInfo* pgfip, PgenReader* simple_pgrp, char* outname, char* outname_end) {
  unsigned char* bigstack_mark = g_bigstack_base;
  PglErr reterr = kPglRetSuccess;
  {
    const uint32_t raw_sample_ctl = BitCtToWordCt(raw_sample_ct);
    const uint32_t sample_ctaw = BitCtToAlignedWordCt(sample_ct);
    const uint32_t sample_ctl = BitCtToWordCt(sample_ct);
    uint32_t* sample_include_cumulative_popcounts;
    uintptr_t* sex_male_collapsed;
    if (bigstack_alloc_u32(raw_sample_ctl, &sample_include_cumulative_popcounts) ||
        bigstack_alloc_w(sample_ctaw, &sex_male_collapsed)) {
      goto Exportf_ret_NOMEM;
    }
    FillCumulativePopcounts(sample_include, raw_sample_ctl, sample_include_cumulative_popcounts);
    CopyBitarrSubset(sex_male, sample_include, sample_ct, sex_male_collapsed);
    ZeroTrailingWords(sample_ctl, sex_male_collapsed);
    uint32_t* sample_missing_geno_cts = nullptr;
    if (exportf_flags & (kfExportfOxGen | kfExportfHaps | kfExportfHapsLegend | kfExportfBgen11 | kfExportfBgen12 | kfExportfBgen13)) {
      if (bigstack_alloc_u32(sample_ct, &sample_missing_geno_cts)) {
        goto Exportf_ret_NOMEM;
      }
    }
    if (exportf_flags & (kfExportf01 | kfExportf12)) {
      // todo
    }
    if (exportf_flags & (kfExportfTypemask - kfExportfIndMajorBed - kfExportfVcf - kfExportfOxGen - kfExportfBgen11 - kfExportfBgen12 - kfExportfBgen13 - kfExportfHaps - kfExportfHapsLegend - kfExportfATranspose - kfExportfA - kfExportfAD)) {
      logerrputs("Error: Only VCF, oxford, bgen-1.x, haps, hapslegend, A, AD, A-transpose, and\nind-major-bed output have been implemented so far.\n");
      reterr = kPglRetNotYetSupported;
      goto Exportf_ret_1;
    }
    const char exportf_delim = (exportf_flags & kfExportfSpaces)? ' ' : '\t';
    if (exportf_flags & kfExportfATranspose) {
      snprintf(outname_end, kMaxOutfnameExtBlen, ".traw");
      PgrClearLdCache(simple_pgrp);
      reterr = Export012Vmaj(outname, sample_include, sample_include_cumulative_popcounts, piip->sii.sample_ids, variant_include, cip, variant_bps, variant_ids, variant_allele_idxs, allele_storage, refalt1_select, variant_cms, sample_ct, piip->sii.max_sample_id_blen, variant_ct, max_allele_slen, exportf_delim, simple_pgrp);
      if (reterr) {
        goto Exportf_ret_1;
      }
    }
    if (exportf_flags & kfExportfIndMajorBed) {
      reterr = ExportIndMajorBed(sample_include, variant_include, variant_allele_idxs, refalt1_select, raw_sample_ct, sample_ct, raw_variant_ct, variant_ct, max_thread_ct, pgr_alloc_cacheline_ct, pgfip, outname, outname_end);
      if (reterr) {
        goto Exportf_ret_1;
      }
    }
    if (exportf_flags & kfExportfOxGen) {
      PgrClearLdCache(simple_pgrp);
      reterr = ExportOxGen(sample_include, sample_include_cumulative_popcounts, sex_male, variant_include, cip, variant_bps, variant_ids, variant_allele_idxs, allele_storage, refalt1_select, sample_ct, variant_ct, max_allele_slen, max_thread_ct, exportf_flags, simple_pgrp, outname, outname_end, sample_missing_geno_cts);
      if (reterr) {
        goto Exportf_ret_1;
      }
    }
    if (exportf_flags & (kfExportfHaps | kfExportfHapsLegend)) {
      PgrClearLdCache(simple_pgrp);
      reterr = ExportOxHapslegend(sample_include, sample_include_cumulative_popcounts, sex_male_collapsed, variant_include, cip, variant_bps, variant_ids, variant_allele_idxs, allele_storage, refalt1_select, sample_ct, raw_variant_ct, variant_ct, max_allele_slen, exportf_flags, simple_pgrp, outname, outname_end);
      if (reterr) {
        goto Exportf_ret_1;
      }
      ZeroU32Arr(sample_ct, sample_missing_geno_cts);
    }
    if (exportf_flags & kfExportfBgen11) {
      assert(PopcountWords(sample_include, raw_sample_ctl) == sample_ct);
      snprintf(outname_end, kMaxOutfnameExtBlen, ".bgen");
      reterr = ExportBgen11(outname, sample_include, sample_include_cumulative_popcounts, sex_male, variant_include, cip, variant_bps, variant_ids, variant_allele_idxs, allele_storage, refalt1_select, sample_ct, raw_variant_ct, variant_ct, max_allele_slen, max_thread_ct, exportf_flags, pgr_alloc_cacheline_ct, pgfip, sample_missing_geno_cts);
      if (reterr) {
        goto Exportf_ret_1;
      }
    } else if (exportf_flags & (kfExportfBgen12 | kfExportfBgen13)) {
      snprintf(outname_end, kMaxOutfnameExtBlen, ".bgen");
      reterr = ExportBgen13(outname, sample_include, sample_include_cumulative_popcounts, &(piip->sii), sex_nm, sex_male, variant_include, cip, variant_bps, variant_ids, variant_allele_idxs, allele_storage, refalt1_select, sample_ct, raw_variant_ct, variant_ct, max_allele_slen, max_thread_ct, exportf_flags, exportf_bits, exportf_id_paste, exportf_id_delim, pgr_alloc_cacheline_ct, pgfip, sample_missing_geno_cts);
      if (reterr) {
        goto Exportf_ret_1;
      }
    }
    if (exportf_flags & (kfExportfOxGen | kfExportfBgen11 | kfExportfBgen12 | kfExportfBgen13 | kfExportfHaps | kfExportfHapsLegend)) {
      snprintf(outname_end, kMaxOutfnameExtBlen, ".sample");
      logprintfww5("Writing %s ... ", outname);
      fflush(stdout);
      uint32_t y_ct = 0;
      const uint32_t y_code = cip->xymt_codes[kChrOffsetY];
      if ((!IsI32Neg(y_code)) && IsSet(cip->chr_mask, y_code)) {
        y_ct = CountChrVariantsUnsafe(variant_include, cip, y_code);
      }
      assert(PopcountWords(sample_include, raw_sample_ctl) == sample_ct);
      reterr = ExportOxSample(outname, sample_include, piip->sii.sample_ids, sample_missing_geno_cts, sex_nm, sex_male, pheno_cols, pheno_names, sample_ct, piip->sii.max_sample_id_blen, pheno_ct, max_pheno_name_blen, variant_ct, y_ct);
      if (reterr) {
        goto Exportf_ret_1;
      }
      logputs("done.\n");
    }
    if (exportf_flags & kfExportfVcf) {
      PgrClearLdCache(simple_pgrp);
      reterr = ExportVcf(sample_include, sample_include_cumulative_popcounts, &(piip->sii), sex_male_collapsed, variant_include, cip, variant_bps, variant_ids, variant_allele_idxs, allele_storage, refalt1_select, pvar_qual_present, pvar_quals, pvar_filter_present, pvar_filter_npass, pvar_filter_storage, pvar_info_reload, xheader_blen, info_flags, sample_ct, raw_variant_ct, variant_ct, max_allele_slen, max_filter_slen, info_reload_slen, max_thread_ct, exportf_flags, exportf_id_paste, exportf_id_delim, xheader, pgfip, simple_pgrp, outname, outname_end);
      if (reterr) {
        goto Exportf_ret_1;
      }
    }
    // todo: everything else
    // sample-major output should share a (probably multithreaded) transpose
    // routine
    if (exportf_flags & (kfExportfA | kfExportfAD)) {
      snprintf(outname_end, kMaxOutfnameExtBlen, ".raw");
      reterr = Export012Smaj(outname, sample_include, piip, sex_nm, sex_male, pheno_cols, variant_include, variant_ids, variant_allele_idxs, allele_storage, refalt1_select, raw_sample_ct, sample_ct, pheno_ct, raw_variant_ct, variant_ct, max_allele_slen, (exportf_flags / kfExportfAD) & 1, (exportf_flags / kfExportfIncludeAlt) & 1, max_thread_ct, pgr_alloc_cacheline_ct, exportf_delim, pgfip);
      if (reterr) {
        goto Exportf_ret_1;
      }
    }

    if ((!(make_plink2_flags & kfMakeFam)) && (exportf_flags & kfExportfIndMajorBed)) {
      snprintf(outname_end, kMaxOutfnameExtBlen, ".fam");
      logprintfww5("Writing %s ... ", outname);
      fflush(stdout);
      reterr = WriteFam(outname, sample_include, piip, sex_nm, sex_male, pheno_cols, nullptr, sample_ct, pheno_ct, exportf_delim);
      if (reterr) {
        goto Exportf_ret_1;
      }
      logputs("done.\n");
    }
    if ((!(make_plink2_flags & kfMakeBim)) && (exportf_flags & kfExportfIndMajorBed)) {
      snprintf(outname_end, kMaxOutfnameExtBlen, ".bim");
      logprintfww5("Writing %s ... ", outname);
      fflush(stdout);
      reterr = WriteMapOrBim(outname, variant_include, cip, variant_bps, variant_ids, variant_allele_idxs, allele_storage, nullptr, refalt1_select, variant_cms, variant_ct, max_allele_slen, exportf_delim, 0, max_thread_ct);
      if (reterr) {
        goto Exportf_ret_1;
      }
      logputs("done.\n");
    }
  }
  while (0) {
  Exportf_ret_NOMEM:
    reterr = kPglRetNomem;
    break;
  }
 Exportf_ret_1:
  BigstackReset(bigstack_mark);
  return reterr;
}

#ifdef __cplusplus
}  // namespace plink2
#endif
