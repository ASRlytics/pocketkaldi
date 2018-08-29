// Created at 2017-03-29

#include "pocketkaldi.h"

#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <algorithm>
#include <string>
#include "am.h"
#include "cmvn.h"
#include "decodable.h"
#include "decoder.h"
#include "fbank.h"
#include "fst.h"
#include "list.h"
#include "nnet.h"
#include "symbol_table.h"
#include "pcm_reader.h"
#include "configuration.h"
#include "util.h"

using pocketkaldi::Decoder;
using pocketkaldi::Fbank;
using pocketkaldi::CMVN;
using pocketkaldi::util::ToRawStatus;
using pocketkaldi::Status;

PKLIST_DEFINE(char, byte_list)

// The internal version of an utterance. It stores the intermediate state in
// decoding.
typedef struct pk_utterance_internal_t {
  pocketkaldi::Vector<float> raw_wave;
} pk_utterance_internal_t;

void pk_init(pk_t *self) {
  self->fst = NULL;
  self->am = NULL;
  self->cmvn_global_stats = NULL;
  self->symbol_table = NULL;
  self->fbank = NULL;
}

void pk_destroy(pk_t *self) {
  delete self->fst;
  self->fst = nullptr;

  delete self->am;
  self->am = NULL;


  if (self->cmvn_global_stats) {
    pk_vector_destroy(self->cmvn_global_stats);
    free(self->cmvn_global_stats);
    self->cmvn_global_stats = NULL;
  }

  if (self->symbol_table) {
    pk_symboltable_destroy(self->symbol_table);
    free(self->symbol_table);
    self->symbol_table = NULL;
  }

  delete self->fbank;
  self->fbank = NULL;
}

// Pocketkaldi model struct
//   FST
//   CMVN
//   TRANS_MODEL
//   AM
//   SYMBOL_TABLE
void pk_load(pk_t *self, const char *filename, pk_status_t *status) {
  pk_readable_t *fd = nullptr;
  pocketkaldi::util::ReadableFile fd_vn;
  std::string fn;
  pocketkaldi::Configuration conf;
  pocketkaldi::Status status_vn;
  status_vn = conf.Read(filename);
  if (!status_vn.ok()) goto pk_load_failed;

  // FST
  fn = conf.GetPathOrElse("fst", "");
  if (fn == "") {
    status_vn = pocketkaldi::Status::Corruption(pocketkaldi::util::Format(
        "Unable to find key 'fst' in {}",
        filename));
    goto pk_load_failed;
  }
  status_vn = fd_vn.Open(fn.c_str());
  self->fst = new pocketkaldi::Fst();
  status_vn = self->fst->Read(&fd_vn);
  if (!status_vn.ok()) goto pk_load_failed;
  fd = NULL;

  // CMVN
  fn = conf.GetPathOrElse("cmvn_stats", "");
  if (fn == "") {
    status_vn = pocketkaldi::Status::Corruption(pocketkaldi::util::Format(
        "Unable to find key 'cmvn_stats' in {}",
        filename));
    goto pk_load_failed;
  }
  fd = pk_readable_open(fn.c_str(), status);
  self->cmvn_global_stats = (pk_vector_t *)malloc(sizeof(pk_vector_t));
  pk_vector_init(self->cmvn_global_stats, 0, NAN);
  pk_vector_read(self->cmvn_global_stats, fd, status);
  if (!status->ok) goto pk_load_failed;
  pk_readable_close(fd);
  fd = NULL;

  // AM
  self->am = new AcousticModel();
  status_vn = self->am->Read(conf);
  if (!status_vn.ok()) goto pk_load_failed;
  fd = NULL;

  // SYMBOL TABLE
  fn = conf.GetPathOrElse("symbol_table", "");
  if (fn == "") {
    status_vn = pocketkaldi::Status::Corruption(pocketkaldi::util::Format(
        "Unable to find key 'symbol_table' in {}",
        filename));
    goto pk_load_failed;
  }
  fd = pk_readable_open(fn.c_str(), status);
  self->symbol_table = (pk_symboltable_t *)malloc(sizeof(pk_symboltable_t));
  pk_symboltable_init(self->symbol_table);
  pk_symboltable_read(self->symbol_table, fd, status);
  if (!status->ok) goto pk_load_failed;
  pk_readable_close(fd);
  fd = NULL;

  // Initialize fbank feature extractor
  self->fbank = new Fbank();

  if (false) {
pk_load_failed:
    if (!status_vn.ok()) {
      PK_STATUS_IOERROR(status, "%s", status_vn.what().c_str());
    } 
    pk_destroy(self);
  }
  if (fd != NULL) pk_readable_close(fd);
}

void pk_utterance_init(pk_utterance_t *utt) {
  utt->internal = new pk_utterance_internal_t;
  utt->hyp = NULL;
  utt->loglikelihood_per_frame = 0.0f;
}

void pk_utterance_destroy(pk_utterance_t *utt) {
  if (utt->internal) {
    delete utt->internal;
    utt->internal = NULL;
  }

  free(utt->hyp);
  utt->hyp = NULL;
  utt->loglikelihood_per_frame = 0.0f;
}

void pk_read_audio(
    pk_utterance_t *utt,
    const char *filename,
    pk_status_t *cs) {
  assert(utt->internal && "pk_read_audio: utterance is not initialized");

  pocketkaldi::Status status = pocketkaldi::Read16kPcm(
      filename, &utt->internal->raw_wave);
  ToRawStatus(status, cs);
}

void pk_process(pk_t *recognizer, pk_utterance_t *utt) {
  assert(utt->hyp == NULL && "utt->hyp == NULL expected");

  // Handle empty utterance
  if (utt->internal->raw_wave.Dim() == 0) {
    utt->hyp = (char *)malloc(sizeof(char));
    *(utt->hyp) = '\0';
    return;
  }

  clock_t t;

  // Extract fbank feats from raw_wave
  t = clock();
  pk_matrix_t raw_feats;
  pk_matrix_init(&raw_feats, 0, 0);
  recognizer->fbank->Compute(utt->internal->raw_wave, &raw_feats);
  t = clock() - t;
  fputs(pocketkaldi::util::Format("Fbank: {}ms\n", ((float)t) / CLOCKS_PER_SEC  * 1000).c_str(), stderr);

  // Apply CMVN to raw_wave
  t = clock();
  CMVN cmvn(recognizer->cmvn_global_stats, &raw_feats);
  pk_matrix_t feats;
  pk_matrix_init(&feats, raw_feats.nrow, raw_feats.ncol);
  for (int frame = 0; frame < raw_feats.ncol; ++frame) {
    pk_vector_t frame_col = pk_matrix_getcol(&feats, frame);
    cmvn.GetFrame(frame, &frame_col);
  }
  t = clock() - t;
  fprintf(stderr, "CMVN: %lfms\n", ((float)t) / CLOCKS_PER_SEC  * 1000);

  // Start to decode
  Decoder decoder(recognizer->fst);
  pk_decodable_t decodable;
  t = clock();
  pk_decodable_init(
      &decodable,
      recognizer->am,
      0.1,
      &feats);
  t = clock() - t;
  fprintf(stderr, "NNET: %lfms\n", ((float)t) / CLOCKS_PER_SEC  * 1000);
  
  // Decoding
  decoder.Decode(&decodable);
  Decoder::Hypothesis hyp = decoder.BestPath();

  // Get final result
  std::string hyp_text;
  std::vector<int> words = hyp.words();
  std::reverse(words.begin(), words.end());
  if (!hyp.words().empty()) {
    for (int word_id : words) {
      // Append the word into hyp
      const char *word = pk_symboltable_get(recognizer->symbol_table, word_id);
      hyp_text += word;
      hyp_text += ' ';
    }

    // Copy hyp to utt->hyp
    utt->hyp = (char *)malloc(sizeof(char) * hyp_text.size());
    pk_strlcpy(utt->hyp, hyp_text.data(), hyp_text.size());
    utt->loglikelihood_per_frame = hyp.weight() / feats.ncol;
  } else {
    utt->hyp = (char *)malloc(sizeof(char));
    *(utt->hyp) = '\0';
  }

  pk_matrix_destroy(&raw_feats);
  pk_matrix_destroy(&feats);
  pk_decodable_destroy(&decodable);
}
