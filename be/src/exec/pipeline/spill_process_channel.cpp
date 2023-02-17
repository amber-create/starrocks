// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "exec/pipeline/spill_process_channel.h"

#include "exec/spill/spiller.h"

namespace starrocks::vectorized {

void SpillProcessTask::reset() {
    _task = {};
}

SpillProcessChannelPtr SpillProcessChannelFactory::get_or_create(int32_t sequence) {
    DCHECK_LT(sequence, _channels.size());
    if (_channels[sequence] == nullptr) {
        _channels[sequence] = std::make_shared<SpillProcessChannel>(this);
    }
    return _channels[sequence];
}
} // namespace starrocks::vectorized