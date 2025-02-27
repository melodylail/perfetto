// Copyright (C) 2021 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {Color} from '../common/colorizer';

export interface Slice {
  // These properties are updated only once per query result when the Slice
  // object is created and don't change afterwards.
  readonly id: number;
  readonly startS: number;
  readonly durationS: number;
  readonly depth: number;
  readonly flags: number;

  // These can be changed by the Impl.
  title: string;
  subTitle: string;
  baseColor: Color;
  color: Color;

  // These properties change @ 60FPS and shouldn't be touched by the Impl.
  // to the Impl. These are really ephemeral and change on every frame. But
  // the Impl doesn't see every frame. Somebody might be tempted to reason on
  // those but then fail.
  // TODO(hjd): Would be nice to find some clever typing hack to avoid exposing
  // these.
  x: number;
  w: number;
}
