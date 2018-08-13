// Changed from feat/wave-reader.cc

// Copyright 2009-2011  Karel Vesely;  Petr Motlicek
//                2013  Florent Masson
//                2013  Johns Hopkins University (author: Daniel Povey)

// See ../../COPYING for clarification regarding multiple authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.

// 2017-1-3

#ifndef POCKETKALDI_PCM_READER_H_
#define POCKETKALDI_PCM_READER_H_

#include <stdint.h>
#include "util.h"
#include "matrix.h"
#include "vector.h"

namespace pocketkaldi {

// Reads 16k sampling rate, mono-channel, PCM formatted wave file, and stores
// the data into pcm_data. If any error occured, set status to failed
Status Read16kPcm(const char *filename, Vector<float> *pcm_data);

}  // namespace pocketkaldi


#endif

