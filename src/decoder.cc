// Created at 2017-04-12

#include "decoder.h"

#include <stdio.h>
#include <time.h>
#include <algorithm>
#include "list.h"
#include "hashtable.h"

namespace pocketkaldi {

Decoder::Token::Token(int state, float cost, int olabel_idx):
    state_(state),
    cost_(cost),
    olabel_idx_(olabel_idx) {
}

Decoder::OLabel::OLabel(int prev_idx, int olabel):
    prev_idx_(prev_idx),
    olabel_(olabel) {
}

Decoder::Hypothesis::Hypothesis(const std::vector<int> &words, float weight):
    words_(words),
    weight_(weight) {
}

Decoder::Decoder(const pk_fst_t *fst):
    fst_(fst),
    beam_(16.0) {
  pk_hashtable_init(&state_idx_, kBeamSize * 4);
}

Decoder::~Decoder() {
  fst_ = nullptr;
  pk_hashtable_destroy(&state_idx_);
}

bool Decoder::Decode(pk_decodable_t *decodable) {
  clock_t t;

  // Extract fbank feats from raw_wave
  clock_t t_all = clock();
  clock_t t_emitting = 0;
  clock_t t_nonemitting = 0;
  
  InitDecoding();
  while(!pk_decodable_islastframe(decodable, num_frames_decoded_ - 1)) {
    t = clock();
    double cutoff = ProcessEmitting( decodable);
    t_emitting += clock() - t;
    
    t = clock();
    ProcessNonemitting(cutoff);
    t_nonemitting = clock() - t;
  }

  t_all = clock() - t_all;
  fprintf(stderr, "decode: %lfms\n", ((float)t_all) / CLOCKS_PER_SEC  * 1000);
  fprintf(stderr, "  process_emitting: %lfms\n", ((float)t_emitting) / CLOCKS_PER_SEC  * 1000);
  fprintf(stderr, "  process_nonemitting: %lfms\n", ((float)t_nonemitting) / CLOCKS_PER_SEC  * 1000);
  return toks_.size() > 0;
}

void Decoder::InitDecoding() {
  // Prepare beams
  toks_.clear();
  prev_toks_.clear();

  // Initialize decoding:
  int start_state = pk_fst_startstate(fst_);
  assert(start_state >= 0);
  
  pk_fst_arc_t dummy_arc;
  dummy_arc.input_label = 0;
  dummy_arc.output_label = 0;
  dummy_arc.next_state = start_state;
  dummy_arc.weight = 0;

  InsertTok(&dummy_arc, kNotExist, 0.0f);
  num_frames_decoded_ = 0;
  ProcessNonemitting(INFINITY);
}

bool Decoder::InsertTok(const pk_fst_arc_t *arc, int olabel_idx, float cost) {
  int next_state = arc->next_state;
  int tok_idx = pk_hashtable_find(&state_idx_, next_state, kNotExist);
  
  // Create the olabel for next tok when the output_label of arc
  // is not 0 (epsilon)
  int next_olabel_idx = kOLabelBeginIdx;
  if (arc->output_label != 0) {
    next_olabel_idx = olabels_.size();
    olabels_.emplace_back(olabel_idx, arc->output_label);
  } else {
    next_olabel_idx = olabel_idx;
  }

  // Insert new or update existing token in the beam
  if (tok_idx == kNotExist) {
    int num_toks = toks_.size();
    toks_.emplace_back(next_state, cost, next_olabel_idx);
    pk_hashtable_upsert(&state_idx_, next_state, num_toks);
  } else {
    // If the cost of existing token is less than the new one, just discard
    // inserting and return false
    if (toks_[tok_idx].cost() > cost) {
      toks_[tok_idx] = Token(next_state, cost, next_olabel_idx);
    } else {
      return false;
    }
  }
  return true;
}

double Decoder::GetCutoff(float *adaptive_beam, int *best_tokidx) {
  double best_cost = INFINITY;
  costs_.clear();
  uint64_t next_random = kCutoffRandSeed;

  // Probability of sample a cost into self->costs
  float sample_prob = kCutoffSamples / (float)toks_.size();
  
  for (int i = 0; i < prev_toks_.size(); ++i) {
    const Token &tok = prev_toks_[i];
    // Random sample costs from beam. To be consistant even in multi-thread
    // environment, do don't use rand() here
    next_random = next_random * (uint64_t)25214903917 + 11;
    float random_f = (next_random & 0xffff) / (float)65535;
    if (random_f < sample_prob) {
      costs_.push_back(tok.cost());
    }

    if (tok.cost() < best_cost) {
      best_cost = tok.cost();
      *best_tokidx = i;
    }
  }

  double beam_cutoff = best_cost + beam_;
  double max_active_cutoff = NAN;
  
  // Here we guess the cutoff weight to limit the prev_toks_ to beam size
  if (prev_toks_.size() > kBeamSize) {
    int cutoff_idx = costs_.size() * kBeamSize / prev_toks_.size();
    std::nth_element(costs_.begin(), costs_.end(), costs_.begin() + cutoff_idx);
    max_active_cutoff = costs_[cutoff_idx];
  }

  if (max_active_cutoff < beam_cutoff) { 
    // max_active_cutoff is tighter than beam.
    *adaptive_beam = max_active_cutoff - best_cost + kBeamDelta;
    beam_cutoff = max_active_cutoff;
  } else {
    *adaptive_beam = beam_;
  }

  return beam_cutoff;
}


// Processes nonemitting arcs for one frame.  Propagates within cur_toks_.
void Decoder::ProcessNonemitting(double cutoff) {
  std::vector<int> queue;
  for (const Token &tok : toks_) {
    queue.push_back(tok.state());
  }

  // Loop until no state in beam have out-going epsilon arc
  while (!queue.empty()) {
    int state = queue.back();
    queue.pop_back();

    // Get tok by state
    int tok_idx = pk_hashtable_find(&state_idx_, state, kNotExist);
    assert(tok_idx != kNotExist);

    pk_fst_iter_t arc_iter;
    pk_fst_iterate_arc(fst_, state, &arc_iter);
    const pk_fst_arc_t *arc = nullptr;
    while ((arc = pk_fst_iter_next(&arc_iter)) != nullptr) {
      // propagate nonemitting only...
      if (arc->input_label != 0) continue;

      const float ac_cost = 0.0;
      const Token &from_tok = toks_[tok_idx];
      double total_cost = from_tok.cost() + arc->weight + ac_cost;
      if (total_cost > cutoff) continue;

      // Create and insert tok into beam
      // If the token successfully inserted or updated in the beam, `inserted`
      // will be true and then we will push the new state into `queue`
      bool inserted = InsertTok(arc, from_tok.olabel_idx(), total_cost);
      if (inserted) queue.push_back(arc->next_state);
    }
  }
}


// Process the emitting (non-epsilon) arcs of each states in the beam
float Decoder::ProcessEmitting(pk_decodable_t *decodable) {
  // Clear the prev_toks_
  prev_toks_.clear();
  pk_hashtable_clear(&state_idx_);

  // Swap toks_ and empty prev_toks_
  toks_.swap(prev_toks_);

  // Calculate beam_cutoff of beam
  float adaptive_beam = INFINITY;
  int best_tokidx = 0;
  float weight_cutoff = GetCutoff(&adaptive_beam, &best_tokidx);

  // This is the cutoff we use after adding in the log-likes (i.e.
  // for the next frame).  This is a bound on the cutoff we will use
  // on the next frame.
  double next_weight_cutoff = INFINITY;

  // First process the best token to get a hopefully
  // reasonably tight bound on the next cutoff.
  const Token &best_tok = prev_toks_[best_tokidx];
  float state = best_tok.state();
  pk_fst_iter_t arc_iter;
  pk_fst_iterate_arc(fst_, state, &arc_iter);
  const pk_fst_arc_t *arc = nullptr;
  while ((arc = pk_fst_iter_next(&arc_iter)) != nullptr) {
    if (arc->input_label == 0) continue;

    float acoustic_cost = -pk_decodable_loglikelihood(
        decodable,
        num_frames_decoded_,
        arc->input_label);
    double total_cost = best_tok.cost() + arc->weight + acoustic_cost;
    if (total_cost + adaptive_beam < next_weight_cutoff) {
      next_weight_cutoff = total_cost + adaptive_beam;
    }
  }
  
  // Ok, we iterate each token in prev_tok_ and add new tokens into toks_ with
  // the emitting arcs of them.
  for (const Token &from_tok : prev_toks_) {
    int state = from_tok.state();

    // weight_cutoff is computed according to beam size
    // So there are only top beam_size toks less than weight_cutoff
    if (from_tok.cost() > weight_cutoff) continue;

    pk_fst_iter_t arc_iter;
    pk_fst_iterate_arc(fst_, state, &arc_iter);
    const pk_fst_arc_t *arc = nullptr;
    while ((arc = pk_fst_iter_next(&arc_iter)) != nullptr) {
      if (arc->input_label == 0) continue;

      float ac_cost = -pk_decodable_loglikelihood(
          decodable,
          num_frames_decoded_,
          arc->input_label);
      double total_cost = from_tok.cost() + arc->weight + ac_cost;
      
      // Prune the toks whose cost is too high
      if (total_cost > next_weight_cutoff) continue;
      if (total_cost + adaptive_beam < next_weight_cutoff) {
        next_weight_cutoff = total_cost + adaptive_beam;
      }

      // Create and insert the tok into toks_
      InsertTok(arc, from_tok.olabel_idx(), total_cost);
    }
  }
  num_frames_decoded_++;
  return next_weight_cutoff;
}

// Get best hypothesis from lattice.
Decoder::Hypothesis Decoder::BestPath() {
  std::vector<int> words;
  float weight;

  // Find the best token
  int best_idx = kNotExist;
  double best_cost = INFINITY;
  for (int i = 0; i < toks_.size(); ++i) {
    const Token &tok = toks_[i];
    int state = tok.state();
    double cost = tok.cost() + pk_fst_final(fst_, state);
    if (cost != INFINITY && cost < best_cost) {
      best_cost = cost;
      best_idx = i;
    }
  }
  if (best_idx == kNotExist) return Hypothesis(std::vector<int>(), 0.0f);

  // Get all output labels from best_tok
  const Token &best_tok = toks_[best_idx];
  int best_olabelidx = best_tok.olabel_idx();
  int olabel_idx = best_olabelidx;
  while (olabel_idx != kOLabelBeginIdx) {
    const OLabel &olabel = olabels_[olabel_idx];
    words.push_back(olabel.olabel());
    olabel_idx = olabel.prev_idx();
  }

  weight = best_cost;
  weight += pk_fst_final(fst_, best_tok.state());

  return Hypothesis(words, weight);
}

}  // namespace pocketkaldi
