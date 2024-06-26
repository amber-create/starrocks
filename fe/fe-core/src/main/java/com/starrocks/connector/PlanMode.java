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

package com.starrocks.connector;

import static com.google.common.base.Preconditions.checkArgument;

public enum PlanMode {
    AUTO("auto"),
    LOCAL("local"),
    DISTRIBUTED("distributed");

    private final String modeName;

    PlanMode(String modeName) {
        this.modeName = modeName;
    }

    public static PlanMode fromName(String modeName) {
        checkArgument(modeName != null, "Mode name is null");

        if (AUTO.modeName().equalsIgnoreCase(modeName)) {
            return AUTO;

        } else if (LOCAL.modeName().equalsIgnoreCase(modeName)) {
            return LOCAL;

        } else if (DISTRIBUTED.modeName().equalsIgnoreCase(modeName)) {
            return DISTRIBUTED;

        } else {
            throw new IllegalArgumentException("Unknown plan mode: " + modeName);
        }
    }

    public String modeName() {
        return modeName;
    }
}