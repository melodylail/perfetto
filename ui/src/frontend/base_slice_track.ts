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

import {assertExists} from '../base/logging';
import {Actions} from '../common/actions';
import {cropText, drawIncompleteSlice} from '../common/canvas_utils';
import {colorCompare, colorToStr, GRAY_COLOR} from '../common/colorizer';
import {NUM, QueryResult} from '../common/query_result';
import {SelectionKind} from '../common/state';
import {fromNs, toNs} from '../common/time';

import {checkerboardExcept} from './checkerboard';
import {globals} from './globals';
import {Slice} from './slice';
import {DEFAULT_SLICE_LAYOUT, SliceLayout} from './slice_layout';
import {NewTrackArgs, SliceRect, Track} from './track';

// The common class that underpins all tracks drawing slices.

export const SLICE_FLAGS_INCOMPLETE = 1;
export const SLICE_FLAGS_INSTANT = 2;

// Slices smaller than this don't get any text:
const SLICE_MIN_WIDTH_FOR_TEXT_PX = 5;
// Slices smaller than this aren't rendered at all.
const SLICE_MIN_WIDTH_PX = 0.1;
const CHEVRON_WIDTH_PX = 10;
const DEFAULT_SLICE_COLOR = GRAY_COLOR;

// TODO(hjd): Implement caching.

// The minimal set of columns that any table/view must expose to render tracks.
// Note: this class assumes that, at the SQL level, slices are:
// - Not temporally overlapping (unless they are nested at inner depth).
// - Strictly stacked (i.e. a slice at depth N+1 cannot be larger than any
//   slices at depth 0..N.
// If you need temporally overlapping slices, look at AsyncSliceTrack, which
// merges several tracks into one visual track.
export const BASE_SLICE_ROW = {
  id: NUM,     // The slice ID, for selection / lookups.
  tsq: NUM,    // Quantized |ts|. This class owns the quantization logic.
  ts: NUM,     // Start time in nanoseconds.
  dur: NUM,    // Duration in nanoseconds. -1 = incomplete, 0 = instant.
  depth: NUM,  // Vertical depth.
};

export type BaseSliceRow = typeof BASE_SLICE_ROW;

// The meta-type which describes the types used to extend the BaseSliceTrack.
// Derived classes can extend this interface to override these types if needed.
export interface BaseSliceTrackTypes {
  slice: Slice;
  row: BaseSliceRow;
  config: {};
}

export abstract class BaseSliceTrack<T extends BaseSliceTrackTypes =
                                                   BaseSliceTrackTypes> extends
    Track<T['config']> {
  // This is the slice cache.
  private slices = new Array<T['slice']>();
  protected sliceLayout: SliceLayout = {...DEFAULT_SLICE_LAYOUT};

  // These are the over-skirted cached bounds.
  private slicesStartNs = -1;
  private slicesEndNs = -1;
  private slicesBucketNs = -1;

  private readonly tableName: string;
  private maxDurNs = 0;
  private sqlState: 'UNINITIALIZED'|'INITIALIZING'|'QUERY_PENDING'|
      'QUERY_DONE' = 'UNINITIALIZED';
  private extraSqlColumns: string[];

  private charWidth = -1;
  private hoverPos?: {x: number, y: number};
  protected hoveredSlice?: T['slice'];
  private hoverTooltip: string[] = [];
  private maxDataDepth = 0;

  // Computed layout.
  private computedTrackHeight = 0;
  private computedSliceHeight = 0;
  private computedRowSpacing = 0;

  // TODO(hjd): Remove when updating selection.
  // We shouldn't know here about CHROME_SLICE. Maybe should be set by
  // whatever deals with that. Dunno the namespace of selection is weird. For
  // most cases in non-ambiguous (because most things are a 'slice'). But some
  // others (e.g. THREAD_SLICE) have their own ID namespace so we need this.
  protected selectionKinds: SelectionKind[] = ['SLICE', 'CHROME_SLICE'];

  // Extension points.
  // Each extension point should take a dedicated argument type (e.g.,
  // OnSliceOverArgs {slice?: T['slice']}) so it makes future extensions
  // non-API-breaking (e.g. if we want to add the X position).
  abstract initSqlTable(_tableName: string): Promise<void>;
  getRowSpec(): T['row'] {
    return BASE_SLICE_ROW;
  }
  onSliceOver(_args: OnSliceOverArgs<T['slice']>): void {}
  onSliceOut(_args: OnSliceOutArgs<T['slice']>): void {}
  onSliceClick(_args: OnSliceClickArgs<T['slice']>): void {}
  prepareSlices(slices: Array<T['slice']>): void {
    this.highlightHovererdAndSameTitle(slices);
  }

  // TODO(hjd): Remove.
  drawSchedLatencyArrow(
      _: CanvasRenderingContext2D, _selectedSlice?: T['slice']): void {}

  constructor(args: NewTrackArgs) {
    super(args);
    this.frontendOnly = true;  // Disable auto checkerboarding.
    this.tableName = `track_${this.trackId}`.replace(/[^a-zA-Z0-9_]+/g, '_');

    // Work out the extra columns.
    // This is the union of the embedder-defined columns and the base columns
    // we know about (ts, dur, ...).
    const allCols = Object.keys(this.getRowSpec());
    const baseCols = Object.keys(BASE_SLICE_ROW);
    this.extraSqlColumns = allCols.filter(key => !baseCols.includes(key));
  }

  setSliceLayout(sliceLayout: SliceLayout) {
    if (sliceLayout.minDepth > sliceLayout.maxDepth) {
      const {maxDepth, minDepth} = sliceLayout;
      throw new Error(`minDepth ${minDepth} must be <= maxDepth ${maxDepth}`);
    }
    this.sliceLayout = sliceLayout;
  }

  onFullRedraw(): void {
    // TODO(hjd): Call this only when cache changes. See discussion:
    // What we want to do here is give the Impl a chance to colour the slice,
    // e.g. depending on the currently selected thread or process.
    // Here's an interesting thought. We have two options here:
    //   A) We could pass only the vizSlices, but then we'd have to call this
    //      @ 60FPS (because vizSlices changes as we pan).
    //   B) We could call this only on full redraws (when the state changes),
    //      but then the track needs to process *all* cached slices, not just
    //      the visible ones. It's okay now (it's a 2x factor) but might get
    //      worse if we cache several layers of slices at various resolutions.
    // But there's an escape, I think. I think the right thing to do is:
    // - For now call it on the full slices, but only on full redraws.
    // - When we get caching, call it every time we switch "cached quantization
    //  level", which is a way in the middle between 60FPS and full redraws..
    // Overall the API contract of this prepareSlices() call is:
    //  - I am going to draw these slices in the near future.
    //  - I am not going to draw any slice that I haven't passed here first.
    //  - This is guaranteed to be called at least on every state change.
    //  - This is NOT guaranteed to be called on every frame. For instance you
    //    cannot use this to do some colour-based animation.

    // Give a chance to the embedder to change colors and other stuff.
    this.prepareSlices(this.slices);
  }

  renderCanvas(ctx: CanvasRenderingContext2D): void {
    // TODO(hjd): fonts and colors should come from the CSS and not hardcoded
    // here.
    const {timeScale} = globals.frontendLocalState;
    const vizTime = globals.frontendLocalState.visibleWindowTime;

    // If the visible time range is outside the cached area, requests
    // asynchronously new data from the SQL engine.
    this.maybeRequestData();

    // In any case, draw whatever we have (which might be stale/incomplete).

    // If the cached trace slices don't fully cover the visible time range,
    // show a gray rectangle with a "Loading..." label.
    checkerboardExcept(
        ctx,
        this.getHeight(),
        timeScale.timeToPx(vizTime.start),
        timeScale.timeToPx(vizTime.end),
        timeScale.timeToPx(fromNs(this.slicesStartNs)),
        timeScale.timeToPx(fromNs(this.slicesEndNs)));

    let charWidth = this.charWidth;
    if (charWidth < 0) {
      // TODO(hjd): Centralize font measurement/invalidation.
      ctx.font = '12px Roboto Condensed';
      charWidth = this.charWidth = ctx.measureText('dbpqaouk').width / 8;
    }

    // Filter only the visible slices. |this.slices| will have more slices than
    // needed because maybeRequestData() over-fetches to handle small pan/zooms.
    // We don't want to waste time drawing slices that are off screen.
    const vizSlices = this.getVisibleSlices(vizTime.start, vizTime.end);

    let selection = globals.state.currentSelection;

    if (!selection || !this.selectionKinds.includes(selection.kind)) {
      selection = null;
    }

    // Believe it or not, doing 4xO(N) passes is ~2x faster than trying to draw
    // everything in one go. The key is that state changes operations on the
    // canvas (e.g., color, fonts) dominate any number crunching we do in JS.

    this.updateSliceAndTrackHeight();
    const sliceHeight = this.computedSliceHeight;
    const padding = this.sliceLayout.padding;
    const rowSpacing = this.computedRowSpacing;

    // First pass: compute geometry of slices.
    let selSlice: T['slice']|undefined;

    // pxEnd is the last visible pixel in the visible viewport. Drawing
    // anything < 0 or > pxEnd doesn't produce any visible effect as it goes
    // beyond the visible portion of the canvas.
    const pxEnd = Math.floor(timeScale.timeToPx(vizTime.end));

    for (const slice of vizSlices) {
      // Compute the basic geometry for any visible slice, even if only
      // partially visible. This might end up with a negative x if the
      // slice starts before the visible time or with a width that overflows
      // pxEnd.
      slice.x = timeScale.timeToPx(slice.startS);
      slice.w = timeScale.deltaTimeToPx(slice.durationS);
      if (slice.flags & SLICE_FLAGS_INSTANT) {
        // In the case of an instant slice, set the slice geometry on the
        // bounding box that will contain the chevron.
        slice.x -= CHEVRON_WIDTH_PX / 2;
        slice.w = CHEVRON_WIDTH_PX;
      } else {
        // If the slice is an actual slice, intersect the slice geometry with
        // the visible viewport (this affects only the first and last slice).
        // This is so that text is always centered even if we are zoomed in.
        // Visually if we have
        //                   [    visible viewport   ]
        //  [         slice         ]
        // The resulting geometry will be:
        //                   [slice]
        // So that the slice title stays within the visible region.
        const sliceVizLimit = Math.min(slice.x + slice.w, pxEnd);
        slice.x = Math.max(slice.x, 0);
        slice.w = sliceVizLimit - slice.x;
      }

      if (selection && (selection as {id: number}).id === slice.id) {
        selSlice = slice;
      }
    }

    // Second pass: fill slices by color.
    // The .slice() turned out to be an unintended pun.
    const vizSlicesByColor = vizSlices.slice();
    vizSlicesByColor.sort((a, b) => colorCompare(a.color, b.color));
    let lastColor = undefined;
    for (const slice of vizSlicesByColor) {
      if (slice.color !== lastColor) {
        lastColor = slice.color;
        ctx.fillStyle = colorToStr(slice.color);
      }
      const y = padding + slice.depth * (sliceHeight + rowSpacing);
      if (slice.flags & SLICE_FLAGS_INSTANT) {
        this.drawChevron(ctx, slice.x, y, sliceHeight);
      } else if (slice.flags & SLICE_FLAGS_INCOMPLETE) {
        const w = Math.max(slice.w - 2, 2);
        drawIncompleteSlice(ctx, slice.x, y, w, sliceHeight);
      } else if (slice.w > SLICE_MIN_WIDTH_PX) {
        ctx.fillRect(slice.x, y, slice.w, sliceHeight);
      }
    }

    // Third pass, draw the titles (e.g., process name for sched slices).
    ctx.fillStyle = '#fff';
    ctx.textAlign = 'center';
    ctx.font = '12px Roboto Condensed';
    ctx.textBaseline = 'middle';
    for (const slice of vizSlices) {
      if ((slice.flags & SLICE_FLAGS_INSTANT) || !slice.title ||
          slice.w < SLICE_MIN_WIDTH_FOR_TEXT_PX) {
        continue;
      }

      const title = cropText(slice.title, charWidth, slice.w);
      const rectXCenter = slice.x + slice.w / 2;
      const y = padding + slice.depth * (sliceHeight + rowSpacing);
      const yDiv = slice.subTitle ? 3 : 2;
      const yMidPoint = Math.floor(y + sliceHeight / yDiv) - 0.5;
      ctx.fillText(title, rectXCenter, yMidPoint);
    }

    // Fourth pass, draw the subtitles (e.g., thread name for sched slices).
    ctx.fillStyle = 'rgba(255, 255, 255, 0.6)';
    ctx.font = '10px Roboto Condensed';
    for (const slice of vizSlices) {
      if (slice.w < SLICE_MIN_WIDTH_FOR_TEXT_PX || !slice.subTitle ||
          (slice.flags & SLICE_FLAGS_INSTANT)) {
        continue;
      }
      const rectXCenter = slice.x + slice.w / 2;
      const subTitle = cropText(slice.subTitle, charWidth, slice.w);
      const y = padding + slice.depth * (sliceHeight + rowSpacing);
      const yMidPoint = Math.ceil(y + sliceHeight * 2 / 3) + 1.5;
      ctx.fillText(subTitle, rectXCenter, yMidPoint);
    }

    // Draw a thicker border around the selected slice (or chevron).
    if (selSlice !== undefined) {
      const color = selSlice.color;
      const y = padding + selSlice.depth * (sliceHeight + rowSpacing);
      ctx.strokeStyle = `hsl(${color.h}, ${color.s}%, 30%)`;
      ctx.beginPath();
      const THICKNESS = 3;
      ctx.lineWidth = THICKNESS;
      ctx.strokeRect(
          selSlice.x, y - THICKNESS / 2, selSlice.w, sliceHeight + THICKNESS);
      ctx.closePath();
    }

    // TODO(hjd): Remove this.
    // The only thing this does is drawing the sched latency arrow. We should
    // have some abstraction for that arrow (ideally the same we'd use for
    // flows).
    this.drawSchedLatencyArrow(ctx, selSlice);

    // If a slice is hovered, draw the tooltip.
    const tooltip = this.hoverTooltip;
    if (this.hoveredSlice !== undefined && tooltip.length > 0 &&
        this.hoverPos !== undefined) {
      if (tooltip.length === 1) {
        this.drawTrackHoverTooltip(ctx, this.hoverPos, tooltip[0]);
      } else {
        this.drawTrackHoverTooltip(ctx, this.hoverPos, tooltip[0], tooltip[1]);
      }
    }  // if (howSlice)
  }

  // This method figures out if the visible window is outside the bounds of
  // the cached data and if so issues new queries (i.e. sorta subsumes the
  // onBoundsChange).
  async maybeRequestData() {
    // Important: this method is async and is invoked on every frame. Care
    // must be taken to avoid piling up queries on every frame, hence the FSM.
    if (this.sqlState === 'UNINITIALIZED') {
      this.sqlState = 'INITIALIZING';

      // TODO(hjd): we need an onDestroy. Right now if you contract and expand a
      // track group this will crash, because the 2nd time we create the track
      // we end up re-issuing the CREATE VIEW table_name.
      // Right now this DROP VIEW is a hack, because it: (1) assumes that
      // tableName is a VIEW and not a TABLE; (2) assume the impl track didn't
      // create any other TABLE/VIEW (which happens to be true right now but
      // might now be in future).
      await this.engine.query(`DROP VIEW IF EXISTS ${this.tableName}`);
      await this.initSqlTable(this.tableName);

      const queryRes = await this.engine.query(`select
          ifnull(max(dur), 0) as maxDur, count(1) as rowCount
          from ${this.tableName}`);
      const row = queryRes.firstRow({maxDur: NUM, rowCount: NUM});
      this.maxDurNs = row.maxDur;
      this.sqlState = 'QUERY_DONE';
    } else if (
        this.sqlState === 'INITIALIZING' || this.sqlState === 'QUERY_PENDING') {
      return;
    }

    const resolutionNs = toNs(globals.getCurResolution());
    const vizTime = globals.frontendLocalState.visibleWindowTime;

    const startNs = toNs(vizTime.start);
    const endNs = toNs(vizTime.end);

    // TODO(hjd): figure out / centralize the resolution steps.
    // Will handle this at the same time as cacheing.
    const bucketNs = resolutionNs;

    if (startNs >= this.slicesStartNs && endNs <= this.slicesEndNs &&
        bucketNs === this.slicesBucketNs) {
      return;  // We have the data already, no need to re-query
    }

    this.sqlState = 'QUERY_PENDING';
    const queryTsq = `(ts + ${bucketNs / 2}) / ${bucketNs} * ${bucketNs}`;

    const extraCols = this.extraSqlColumns.join(',');
    let depthCol = 'depth';
    let maybeGroupByDepth = 'depth, ';
    const layout = this.sliceLayout;
    const isFlat = (layout.maxDepth - layout.minDepth) <= 1;
    // maxDepth === minDepth only makes sense if track is empty which on the
    // one hand isn't very useful (and so maybe should be an error) on the
    // other hand I can see it happening if someone does:
    // minDepth = min(slices.depth); maxDepth = max(slices.depth);
    // and slices is empty, so we treat that as flat.
    if (isFlat) {
      depthCol = `${this.sliceLayout.minDepth} as depth`;
      maybeGroupByDepth = '';
    }

    // TODO(hjd): Re-reason and improve this query:
    // - Materialize the unfinished slices one off.
    // - Avoid the union if we know we don't have any -1 slices.
    // - Maybe we don't need the union at all and can deal in TS?
    const queryRes = await this.engine.query(`
    with q1 as (
      select
        ${queryTsq} as tsq,
        ts,
        max(dur) as dur,
        id,
        ${depthCol}
        ${extraCols ? ',' + extraCols : ''}
      from ${this.tableName}
      where
        ts >= ${startNs - this.maxDurNs /* - durNs */} and
        ts <= ${endNs /* + durNs */}
      group by ${maybeGroupByDepth} tsq
      order by tsq),
    q2 as (
      select
        ${queryTsq} as tsq,
        ts,
        -1 as dur,
        id,
        ${depthCol}
        ${extraCols ? ',' + extraCols : ''}
      from ${this.tableName}
      where dur = -1
      group by ${maybeGroupByDepth} tsq
      )
      select min(dur) as _unused, * from
      (select * from q1 union all select * from q2)
      group by ${maybeGroupByDepth} tsq
      order by tsq
    `);
    this.convertQueryResultToSlices(queryRes, startNs, endNs, bucketNs);
    this.sqlState = 'QUERY_DONE';
    globals.rafScheduler.scheduleRedraw();
  }

  // Here convert each row to a Slice. We do what we can do generically
  // in the base class, and delegate the rest to the impl via that rowToSlice()
  // abstract call.
  convertQueryResultToSlices(
      queryRes: QueryResult, startNs: number, endNs: number, bucketNs: number) {
    const slices = new Array<T['slice']>(queryRes.numRows());
    const it = queryRes.iter(this.getRowSpec());

    let maxDataDepth = this.maxDataDepth;
    this.slicesStartNs = startNs;
    this.slicesEndNs = endNs;
    this.slicesBucketNs = bucketNs;
    for (let i = 0; it.valid(); it.next(), ++i) {
      maxDataDepth = Math.max(maxDataDepth, it.depth);

      // Construct the base slice. The Impl will construct and return the full
      // derived T["slice"] (e.g. CpuSlice) in the rowToSlice() method.
      slices[i] = this.rowToSlice(it);
    }
    this.maxDataDepth = maxDataDepth;
    this.slices = slices;
  }

  rowToSlice(row: T['row']): T['slice'] {
    const startNsQ = row.tsq;
    const startNs = row.ts;
    let flags = 0;
    let durNs: number;
    if (row.dur === -1) {
      durNs = toNs(globals.state.traceTime.endSec) - startNs;
      flags |= SLICE_FLAGS_INCOMPLETE;
    } else {
      flags |= (row.dur === 0) ? SLICE_FLAGS_INSTANT : 0;
      durNs = row.dur;
    }
    const endNs = startNs + durNs;
    const bucketNs = this.slicesBucketNs;
    let endNsQ = Math.floor((endNs + bucketNs / 2 - 1) / bucketNs) * bucketNs;
    endNsQ = Math.max(endNsQ, startNsQ + bucketNs);

    return {
      id: row.id,
      startS: fromNs(startNsQ),
      durationS: fromNs(endNsQ - startNsQ),
      flags,
      depth: row.depth,
      title: '',
      subTitle: '',

      // The derived class doesn't need to initialize these. They are
      // rewritten on every renderCanvas() call. We just need to initialize
      // them to something.
      baseColor: DEFAULT_SLICE_COLOR,
      color: DEFAULT_SLICE_COLOR,
      x: -1,
      w: -1,
    };
  }

  private findSlice({x, y}: {x: number, y: number}): undefined|Slice {
    const trackHeight = this.computedTrackHeight;
    const sliceHeight = this.computedSliceHeight;
    const padding = this.sliceLayout.padding;
    const rowSpacing = this.computedRowSpacing;

    // Need at least a draw pass to resolve the slice layout.
    if (sliceHeight === 0) {
      return undefined;
    }

    if (y >= padding && y <= trackHeight - padding) {
      const depth = Math.floor((y - padding) / (sliceHeight + rowSpacing));
      for (const slice of this.slices) {
        if (slice.depth === depth && slice.x <= x && x <= slice.x + slice.w) {
          return slice;
        }
      }
    }

    return undefined;
  }

  onMouseMove(position: {x: number, y: number}): void {
    this.hoverPos = position;
    this.updateHoveredSlice(this.findSlice(position));
  }

  onMouseOut(): void {
    this.updateHoveredSlice(undefined);
  }

  private updateHoveredSlice(slice?: T['slice']): void {
    const lastHoveredSlice = this.hoveredSlice;
    this.hoveredSlice = slice;

    // Only notify the Impl if the hovered slice changes:
    if (slice === lastHoveredSlice) return;

    if (this.hoveredSlice === undefined) {
      globals.dispatch(Actions.setHighlightedSliceId({sliceId: -1}));
      this.onSliceOut({slice: assertExists(lastHoveredSlice)});
      this.hoverTooltip = [];
      this.hoverPos = undefined;
    } else {
      const args: OnSliceOverArgs<T['slice']> = {slice: this.hoveredSlice};
      globals.dispatch(
          Actions.setHighlightedSliceId({sliceId: this.hoveredSlice.id}));
      this.onSliceOver(args);
      this.hoverTooltip = args.tooltip || [];
    }
  }

  onMouseClick(position: {x: number, y: number}): boolean {
    const slice = this.findSlice(position);
    if (slice === undefined) {
      return false;
    }
    const args: OnSliceClickArgs<T['slice']> = {slice};
    this.onSliceClick(args);
    return true;
  }

  getVisibleSlices(startS: number, endS: number): Array<T['slice']> {
    let startIdx = -1;
    let endIdx = -1;
    let i = 0;

    // TODO(hjd): binary search.
    for (const slice of this.slices) {
      if (startIdx < 0 && slice.startS + slice.durationS >= startS) {
        startIdx = i;
      }
      if (slice.startS <= endS) {
        endIdx = i + 1;
      } else if (slice.startS > endS) {
        endIdx = i;
        break;
      }
      i++;
    }
    return this.slices.slice(startIdx, endIdx);
  }

  private updateSliceAndTrackHeight() {
    const lay = this.sliceLayout;

    const rows =
        Math.min(Math.max(this.maxDataDepth + 1, lay.minDepth), lay.maxDepth);

    // Compute the track height.
    let trackHeight;
    if (lay.heightMode === 'FIXED') {
      trackHeight = lay.fixedHeight;
    } else {
      trackHeight = 2 * lay.padding + rows * (lay.sliceHeight + lay.rowSpacing);
    }

    // Compute the slice height.
    let sliceHeight: number;
    let rowSpacing: number = lay.rowSpacing;
    if (lay.heightMode === 'FIXED') {
      const rowHeight = (trackHeight - 2 * lay.padding) / rows;
      sliceHeight = Math.floor(Math.max(rowHeight - lay.rowSpacing, 0.5));
      rowSpacing = Math.max(lay.rowSpacing, rowHeight - sliceHeight);
      rowSpacing = Math.floor(rowSpacing * 2) / 2;
    } else {
      sliceHeight = lay.sliceHeight;
    }
    this.computedSliceHeight = sliceHeight;
    this.computedTrackHeight = trackHeight;
    this.computedRowSpacing = rowSpacing;
  }

  private drawChevron(
      ctx: CanvasRenderingContext2D, x: number, y: number, h: number) {
    // Draw an upward facing chevrons, in order: A, B, C, D, and back to A.
    // . (x, y)
    //      A
    //     ###
    //    ##C##
    //   ##   ##
    //  D       B
    //            . (x + CHEVRON_WIDTH_PX, y + h)
    const HALF_CHEVRON_WIDTH_PX = CHEVRON_WIDTH_PX / 2;
    const midX = x + HALF_CHEVRON_WIDTH_PX;
    ctx.beginPath();
    ctx.moveTo(midX, y);                              // A.
    ctx.lineTo(x + CHEVRON_WIDTH_PX, y + h);          // B.
    ctx.lineTo(midX, y + h - HALF_CHEVRON_WIDTH_PX);  // C.
    ctx.lineTo(x, y + h);                             // D.
    ctx.lineTo(midX, y);                              // Back to A.
    ctx.closePath();
    ctx.fill();
  }

  // This is a good default implemenation for highlighting slices. By default
  // prepareSlices() calls this. However, if the XxxSliceTrack impl overrides
  // prepareSlices() this gives them a chance to call the highlighting witout
  // having to reimplement it.
  protected highlightHovererdAndSameTitle(slices: Slice[]) {
    for (const slice of slices) {
      const isHovering = globals.state.highlightedSliceId === slice.id ||
          (this.hoveredSlice && this.hoveredSlice.title === slice.title);
      if (isHovering) {
        slice.color = {
          c: slice.baseColor.c,
          h: slice.baseColor.h,
          s: slice.baseColor.s,
          l: 30
        };
      } else {
        slice.color = slice.baseColor;
      }
    }
  }

  getHeight(): number {
    this.updateSliceAndTrackHeight();
    return this.computedTrackHeight;
  }

  getSliceRect(_tStart: number, _tEnd: number, _depth: number): SliceRect
      |undefined {
    // TODO(hjd): Implement this as part of updating flow events.
    return undefined;
  }
}

// This is the argument passed to onSliceOver(args).
// This is really a workaround for the fact that TypeScript doesn't allow
// inner types within a class (whether the class is templated or not).
export interface OnSliceOverArgs<S extends Slice> {
  // Input args (BaseSliceTrack -> Impl):
  slice: S;  // The slice being hovered.

  // Output args (Impl -> BaseSliceTrack):
  tooltip?: string[];  // One entry per row, up to a max of 2.
}

export interface OnSliceOutArgs<S extends Slice> {
  // Input args (BaseSliceTrack -> Impl):
  slice: S;  // The slice which is not hovered anymore.
}

export interface OnSliceClickArgs<S extends Slice> {
  // Input args (BaseSliceTrack -> Impl):
  slice: S;  // The slice which is clicked.
}
