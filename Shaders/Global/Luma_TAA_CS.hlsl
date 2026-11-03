#include "../Includes/Color.hlsl"
#include "../Includes/Math.hlsl"
#include "../Includes/Reinhard.hlsl"

// Luma TAA: temporal anti-aliasing resolve, native or upscaling ("TAA_UPSCALE"), for games without usable TAA or as a
// vendor-agnostic alternative to DLAA and FSR Native AA. Components and the sources of their ideas:
// - bicubic history: MJP's Catmull-Rom with the 5 taps of Jimenez (SIGGRAPH 2016), renormalized; an exact 4x4 for the
//   2x2 history
// - motion vector of the closest 3x3 depth (Karis, SIGGRAPH 2014)
// - rounded 3x3 + cross box, history clipped toward its clamped average (Playdead's INSIDE)
// - linear blend, which keeps the energy of HDR highlights, optionally with Reinhard around it (Godot 4)
// - accumulated weight 1 / (2 - w) (MiniEngine), capped by the resampling blur (Yang et al., SIGGRAPH Asia 2009)
// - thin feature lock, reactive mask, deringing of the 2x2 history, and at Ultra a depth clip against the reconstructed
//   previous depth (AMD FSR 2)
// - flickering analysis and the 2x2 history (Unreal Engine's TSR, ideas only, no Unreal Engine code)
// - shading change detection (AMD FSR 3.1)
//
// Inputs follow "SR::SuperResolutionImpl::DrawData": device depth, motion vectors that "MotionVectorScale" turns into
// pixels (previous = current + mv), linear BT.709 color (scRGB is fine). The history is a Luma owned RGBA16F texture:
// rgb is linear color, alpha the accumulated weight; 2x2 the render resolution from High quality.
//
// MIT License. Copyright (c) 2026 Hlib Omelchenko. Parts are derived from third party code, also under the MIT License:
// - "SampleHistory" and "BicubicWeights": MJP's "SampleTextureCatmullRom". Copyright (c) 2019 MJP
// - "ClipToBox" and the rounded box: Playdead's "temporal". Copyright (c) 2015 Playdead
// - the weight rcp(2 - w): Microsoft MiniEngine's "TemporalBlendCS.hlsl". Copyright (c) 2015 Microsoft
// - the previous depth reconstruction (in "shading_change_cs") and the depth clip: AMD FidelityFX FSR 2. Copyright (c)
//   2022-2023 Advanced Micro Devices, Inc. All rights reserved.
// - "shading_change_cs", "SortCross", "SmallestCrossDifference" and the shading change's use: AMD FidelityFX FSR 3.1.
//   Copyright (c) 2023-2024 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
// Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

// Quality levels: 0 Low: 3x3 box, closest depth of the cross, bilinear history. 1 Medium: + 5-tap bicubic history,
// closest depth of the full 3x3, flickering analysis, reactive mask, depth clip against the previous frame's depth.
// 2 High: + 2x2 history. 3 Ultra: + depth clip against the reconstructed previous depth
// (reconstructed by "shading_change_cs", dispatched first).
#ifndef TAA_QUALITY
#define TAA_QUALITY 2
#endif
// Upper bound of the history weight: at least 4% of every frame is kept (an effective window of ~25 frames), which
// bounds lag on lighting and shading changes that the neighborhood box can't catch.
#ifndef TAA_MAX_HISTORY_WEIGHT
#define TAA_MAX_HISTORY_WEIGHT 0.96
#endif
// Resampling the history at a fractional position blurs it every frame, by an amount set by the fractional part of the
// displacement (none at whole pixels, most at half pixels), not by the speed. The history weight is capped at
// tolerance / (tolerance + blur) so that blur stays bounded (Yang et al. 2009). The cap applies to this frame only: the
// accumulated weight keeps converging to it, so motion never compounds into the stored weight.
#ifndef TAA_RESAMPLING_BLUR_TOLERANCE
#define TAA_RESAMPLING_BLUR_TOLERANCE 0.4
#endif
// 0 blends linearly: the history converges to the mean of the jittered samples, so sub-pixel HDR highlights keep their
// energy (77-90% in the lab's glint scenes with the lock below, the rest is lost to clipping on frames where the jitter
// misses them), at the cost of more twinkle. 1 blends in Reinhard space, with a peak of 1 / "TAA_TONEMAP_SCALE".
// Bright samples (sub-pixel HDR highlights, fireflies) then barely move the average, which steadies them but dims them
// (16-31% of their energy is left at a scale of 0.25), as averaging in a compressed space biases toward darker samples.
// This only applies to the render resolution history.
#ifndef TAA_REINHARD_BLEND
#define TAA_REINHARD_BLEND 0
#endif
#ifndef TAA_TONEMAP_SCALE
#define TAA_TONEMAP_SCALE 0.25
#endif
// Ultra quality depth clip: history is kept in proportion to tolerance * depth / separation when the current closest
// surface is farther than the nearest surface that moved to the reprojected position (a disocclusion). Relative linear
// depth, tuned on the lab's layered scenes; AMD FSR 2 derives a resolution-scaled one (~5% of depth at 1080p).
#ifndef TAA_DEPTH_TOLERANCE
#define TAA_DEPTH_TOLERANCE 0.3
#endif
// Depth clip from Medium quality without a reconstruction pass: a disocclusion is where this frame's closest 3x3 depth is
// farther than the previous frame's farthest 3x3 depth at the reprojected pixel (kept in the lock texture). The closest
// against the farthest is blind to a near feature that the jitter hits on some frames only, which never fills a whole
// 3x3, at the cost of the outermost pixel of a disoccluded band. The two depths are a frame apart, so a surface that
// recedes (a camera moving back) separates too: the history is kept in proportion to tolerance * depth / separation,
// with a tolerance above any plausible recession per frame. Ultra keeps the stricter of this and its own depth clip,
// whose wider tolerance lets this one catch more of the separations it misses.
#ifndef TAA_PREVIOUS_DEPTH_CLIP
#define TAA_PREVIOUS_DEPTH_CLIP (TAA_QUALITY >= 1)
#endif
#ifndef TAA_PREVIOUS_DEPTH_TOLERANCE
#define TAA_PREVIOUS_DEPTH_TOLERANCE 0.15
#endif
// Thin feature lock: the neighborhood box is built from the current frame only, so a sub-pixel highlight that this frame's
// jitter missed is clipped out of the history, and the highlight twinkles and loses energy. A pixel whose relative
// luminance is above TAA_LOCK_MIN_LUMINANCE (in the input's linear units) and TAA_LOCK_RATIO times its neighbors' highest
// locks for TAA_LOCK_FRAMES frames (the jitter period: a highlight hit once per cycle stays locked), reprojected with the
// history, and locked history isn't clipped. Only on nearly static pixels (under TAA_LOCK_MAX_SPEED pixels per frame,
// both to create and to use a lock): a moving feature uncovers background whose reprojection lands on its old lock, and
// unclipped history would trail behind it. A pixel that moves that fast ends its lock at once (its slot stores its speed,
// see TAA_LOCK_SPEED_STEPS).
#ifndef TAA_LOCK
#define TAA_LOCK 1
#endif
#ifndef TAA_LOCK_RATIO
#define TAA_LOCK_RATIO 2.0
#endif
#ifndef TAA_LOCK_MIN_LUMINANCE
#define TAA_LOCK_MIN_LUMINANCE 1.0
#endif
#ifndef TAA_LOCK_FRAMES
#define TAA_LOCK_FRAMES 16
#endif
#ifndef TAA_LOCK_MAX_SPEED
#define TAA_LOCK_MAX_SPEED 0.1
#endif
// The lock texture's low byte also carries the speed of pixels that moved at least TAA_LOCK_MAX_SPEED (they hold no lock),
// for the flickering analysis: TAA_LOCK_FRAMES + 1 + the speed in steps of 1 / TAA_LOCK_SPEED_STEPS pixels per frame.
// Without TAA_LOCK nothing is stored, and the flickering range restarts wherever a pixel moves faster than
// TAA_FLICKER_MOTION_CHANGE.
#ifndef TAA_LOCK_SPEED_STEPS
#define TAA_LOCK_SPEED_STEPS 16.0
#endif
// Temporal upscaling (TAAU): the inputs are at "RenderResolution", while the history, the per pixel states and the
// output are at "OutputResolution", with one thread per output pixel. Each output pixel takes this frame's sample only
// when it fell inside it (the box splat of the 2x history, at the output pixel's size), otherwise a jitter-aware
// Gaussian of the 3x3 render pixels ("TAA_UPSCALE_SIGMA" output pixels, weighted by "TAA_UPSCALE_FALLBACK_WEIGHT").
// The history weight caps are scaled by the share of frames in which an output pixel receives a sample (render /
// output pixel area).
#ifndef TAA_UPSCALE
#define TAA_UPSCALE 0
#endif
// An output pixel without a sample this frame still takes the Gaussian estimate, at this weight times its nearest
// sample's Gaussian weight. This leaves fewer frames without any current information in motion, at the cost of a little
// sharpness.
#ifndef TAA_UPSCALE_FALLBACK_WEIGHT
#define TAA_UPSCALE_FALLBACK_WEIGHT 0.5
#endif
// The upscaled history's resampling blur tolerance, in output pixels (the 2x history's is 0.2)
#ifndef TAA_UPSCALE_BLUR_TOLERANCE
#define TAA_UPSCALE_BLUR_TOLERANCE 0.4
#endif
#ifndef TAA_UPSCALE_SIGMA
#define TAA_UPSCALE_SIGMA 0.5
#endif
// Sparse sampling (AMD FSR 2/3 upscaler ideas): from an upscale ratio of "TAA_UPSCALE_SPARSE_FULL" (faded in from
// "TAA_UPSCALE_SPARSE_START"), an output pixel gets its own sample too rarely for the box splat and the per sample cap:
// - each render sample of the 3x3 is weighted by a Lanczos 2 of its distance in output pixels (times
//   "TAA_UPSCALE_LANCZOS_SCALE", narrower below 2x), normalized by the kernel's integral (0.74) so a frame weighs as much
//   as the box splat on average, and clamped to the neighborhood box (deringing);
// - static pixels keep a history cap per output pixel instead of per sample, and their clip box is scaled by
//   "TAA_UPSCALE_STATIC_BOX" around its center (not where the flickering analysis saw the lighting change), both
//   returning to the moving behavior at "TAA_UPSCALE_STATIC_SPEED" output pixels per frame.
// Lab: 2x and 3x better box and FLIP scores on the training and validation scenes, 1-2% worse on stress; at 1.5x the box
// score got worse (more flicker), so it's left out there. Low quality keeps the box splat (worse FLIP without the
// flickering analysis and lock).
#ifndef TAA_UPSCALE_SPARSE
#define TAA_UPSCALE_SPARSE (TAA_QUALITY >= 1)
#endif
#ifndef TAA_UPSCALE_SPARSE_START
#define TAA_UPSCALE_SPARSE_START 1.5
#endif
#ifndef TAA_UPSCALE_SPARSE_FULL
#define TAA_UPSCALE_SPARSE_FULL 2.0
#endif
#ifndef TAA_UPSCALE_LANCZOS_SCALE
#define TAA_UPSCALE_LANCZOS_SCALE 1.25
#endif
#ifndef TAA_UPSCALE_STATIC_BOX
#define TAA_UPSCALE_STATIC_BOX 3.0
#endif
#ifndef TAA_UPSCALE_STATIC_SPEED
#define TAA_UPSCALE_STATIC_SPEED 1.0
#endif
// History at 2x2 the render resolution (UE TSR's "r.TSR.History.ScreenPercentage" 200), from High quality: repeated
// reprojection blurs it far less. Each thread resolves its pixel's 4 history texels and outputs their mean. A texel
// takes this frame's sample only when it fell in its quarter pixel. This is a box splat, so the mean of the 4 texels
// converges to the pixel's box filter. A texel without a sample falls back to a jitter-aware Gaussian of the 3x3 (sigma
// "TAA_HISTORY_2X_SIGMA" render pixels), but only where its history is rejected or under the reactive mask. Where the
// pixel's own sample is under the mask, its 4 texels move toward that Gaussian around the pixel center, as much as the
// mask: content without motion vectors makes per texel estimates from neighboring samples flicker. The mean of
// the 4 history texels is clipped, and the texels keep their deviations from it (the sub-pixel detail), scaled by the
// mean's clip factor. The history textures must match Core's "LumaTAA::history_2x_quality".
#ifndef TAA_HISTORY_2X
#define TAA_HISTORY_2X (TAA_QUALITY >= 2 && !TAA_UPSCALE)
#endif
// Bicubic history sharpness c (Catmull-Rom is 0.5). A 2x history texel gets a sample every 4th frame but is resampled
// every frame, so it compounds its filter's response about 4x: there 0.6 kept the most texture detail in a pan but rang
// on alpha tested cutouts and sliding highlights in motion, and 0.4 lost detail.
#ifndef TAA_HISTORY_SHARPNESS
#define TAA_HISTORY_SHARPNESS 0.5
#endif
// The 2x history's resampling blur tolerance, in history texels. This is the lab optimum, the render resolution history
// uses "TAA_RESAMPLING_BLUR_TOLERANCE".
#ifndef TAA_HISTORY_2X_BLUR_TOLERANCE
#define TAA_HISTORY_2X_BLUR_TOLERANCE 0.2
#endif
// Where the closest depth's motion (the one the history is reprojected with) isn't the pixel's own, one side's history is
// moved with the other side's motion, and the bicubic's overshoot there rings around HDR highlights. Such texels are
// clamped to their 2x2 nearest history texels (deringing, as AMD FSR 2 does for its Lanczos), fully from this motion
// difference in pixels per frame. The other histories don't need it: they clip each pixel, while the 2x one clips its
// texels' mean and keeps their deviations.
#ifndef TAA_HISTORY_2X_DERING_MOTION
#define TAA_HISTORY_2X_DERING_MOTION 0.1
#endif
// Share of frames in which a history texel receives a sample (1/4 for the box splat). The history weight caps are
// defined for a texel sampled every frame, so they are scaled by this to keep the same time constant for every texel.
#ifndef TAA_HISTORY_2X_RATE
#define TAA_HISTORY_2X_RATE 0.25
#endif
#ifndef TAA_HISTORY_2X_SIGMA
#define TAA_HISTORY_2X_SIGMA 0.3
#endif
// Under the reactive mask, a texel without a sample this frame also takes the Gaussian, with a weight of this value
// times the mask times the Gaussian's peak tap. The mask only lowers the history's cap, so such a texel would otherwise
// keep its history in full ("history / (history + 0)" is 1) until a sample lands in it, every 4th frame on average.
#ifndef TAA_HISTORY_2X_REACTIVE_FALLBACK
#define TAA_HISTORY_2X_REACTIVE_FALLBACK 0.25
#endif
// Under the reactive mask the masked pixel's Gaussian (see "TAA_HISTORY_2X") counts as a full sample in all 4 texels,
// weighted by the pixel's own mask: the fallback alone left the 3 texels without a sample at ~85% history under a
// faint mask (0.4), so blinking additive lights (Bayonetta's laser gates) blinked far less than in AMD FSR 3. 0 disables.
#ifndef TAA_HISTORY_2X_REACTIVE_SAMPLE
#define TAA_HISTORY_2X_REACTIVE_SAMPLE 1
#endif
// Deferred accumulation (AMD FSR 3.1's "fMinDisocclusionAccumulation" idea): a texel without a sample this frame whose
// history this frame holds less than "TAA_HISTORY_2X_DEFERRED_SAMPLES" samples (fresh after a rejection, such as content
// entering at the screen edge) also takes the Gaussian, with a weight of this value times the Gaussian's peak tap, fading
// out as its history reaches that many samples: the Gaussians of a few frames average out before its first samples land.
// Above ~0.25 samples it also fires in motion (the resampling cap goes down to 0.2 samples there) and blurs it. It costs
// a little FLIP around sub-pixel HDR glints on a dark background (their energy stays). 0 disables it.
#ifndef TAA_HISTORY_2X_DEFERRED
#define TAA_HISTORY_2X_DEFERRED 0.05
#endif
#ifndef TAA_HISTORY_2X_DEFERRED_SAMPLES
#define TAA_HISTORY_2X_DEFERRED_SAMPLES 0.25
#endif
// Flickering analysis (after UE TSR's): the 3x3 clip against a current frame that aliases (detail near the Nyquist
// limit, sub-pixel shading, alpha tested cutouts) throws away the history that would average the aliasing out, and the
// output flickers. Each pixel keeps the range (min/max) its current sample's luminance has been seen in, reprojected with
// the history and decaying toward the current value by TAA_FLICKER_ENVELOPE_DECAY per frame, and the clip box is widened
// by TAA_FLICKER_GAMMA times that range: on a static pixel that alternates between two values (a cutout appearing and
// vanishing between jitter phases), the history average of the two stays inside the box.
// Off at every level: the widened box kept the old history of shading that changes without motion vectors (moving shadows,
// particles) and ghosted it in game, which the shading change detection didn't fully catch.
#ifndef TAA_FLICKER
#define TAA_FLICKER 0
#endif
#ifndef TAA_FLICKER_GAMMA
#define TAA_FLICKER_GAMMA 1.0
#endif
#ifndef TAA_FLICKER_ENVELOPE_DECAY
#define TAA_FLICKER_ENVELOPE_DECAY 0.25
#endif
// The range starts over where the lighting changed, which a range alone can't tell from aliasing. That is when the
// luminance of the 3x3 neighborhood's darkest sample changes by more than "TAA_FLICKER_RESET_CHANGE" (relative) against
// its own exponential average ("TAA_FLICKER_RESET_SMOOTHING" per frame), and its mean luminance by more than
// "TAA_FLICKER_RESET_MEAN_CHANGE" the same way. "TAA_FLICKER_RESET_CHANGE" 0 disables the test. A bright cutout or sub-pixel detail moves the 3x3 mean
// and max by up to 30x as it appears and vanishes, but the background around it keeps the min. A dark one (a black seam or
// gap, a hole over a dark void) moves the min as much, which made static pixels lose their range at the jitter's period,
// but the mean far less. A lighting change moves both, the same way.
#ifndef TAA_FLICKER_RESET_CHANGE
#define TAA_FLICKER_RESET_CHANGE 0.9
#endif
#ifndef TAA_FLICKER_RESET_SMOOTHING
#define TAA_FLICKER_RESET_SMOOTHING 0.25
#endif
#ifndef TAA_FLICKER_RESET_MEAN_CHANGE
#define TAA_FLICKER_RESET_MEAN_CHANGE 0.5
#endif
// The range also starts over where the pixel's speed differs by more than this (pixels per frame) from the speed its
// reprojected state was written with: the state then belongs to another surface (an object that passed over the
// background, or the background it uncovered). A surface that keeps moving (a camera pan) keeps its range. At least
// 2 / TAA_LOCK_SPEED_STEPS: speeds under TAA_LOCK_MAX_SPEED read back as 0, and 0.1 is stored as 0.125.
// Sub-pixel highlights (HDR glints) are the exception: one that moves sets the 3x3's closest depth, and so the speed,
// only on the frames the jitter hits it, and restarting its range on the others clipped its history (the 1080p
// drifting glints kept 86% of their energy). Where the history is a highlight (the lock's criterion against the 3x3),
// a speed drop from under "TAA_FLICKER_DROP_SPEED" keeps the range; instead the range restarts on static pixels next
// to a moving highlight brighter than them, where the history lags behind the highlight or rings around it.
#ifndef TAA_FLICKER_MOTION_CHANGE
#define TAA_FLICKER_MOTION_CHANGE 0.125
#endif
#ifndef TAA_FLICKER_DROP_SPEED
#define TAA_FLICKER_DROP_SPEED 0.5
#endif
// The widening fades out with motion, reaching none at this speed (pixels per frame): fast content reprojects history the
// resampling blurred, which a tight clip keeps correcting (UE TSR disables its analysis on fast parallax too).
// 0 disables the fade.
#ifndef TAA_FLICKER_MAX_SPEED
#define TAA_FLICKER_MAX_SPEED 1.0
#endif
// A step (other content under a static pixel without a reset: a camera cut, a particle without motion vectors) takes this
// frame's sample out of the range, which then spans both contents and widens the box around the old history. A cutout the
// range forgot between two hits (one per jitter cycle) does the same, but its average is still in the history, outside
// the forgotten range, while the history of a step is the old content, inside the range. So where the history is inside
// the previous frame's range (give or take "TAA_FLICKER_STEP_MARGIN" of it), this frame widens the box by at most
// "TAA_FLICKER_STEP_GROWTH" times that range, and beyond twice that the range starts over at this frame's sample. 0
// disables both. Not with "TAA_UPSCALE" (not measured there).
#ifndef TAA_FLICKER_STEP_GROWTH
#define TAA_FLICKER_STEP_GROWTH 2.0
#endif
#ifndef TAA_FLICKER_STEP_MARGIN
#define TAA_FLICKER_STEP_MARGIN 0.25
#endif
// Reactive mask input (AMD FSR 2 semantics, t7), drawn for example by a game's alpha blended particles. Where it is set,
// the history weight cap is scaled by 1 - mask (with the mask capped at "TAA_REACTIVE_MAX"), the flickering range starts
// over and nothing locks. It's only read when "TAA_FLAG_REACTIVE_MASK" says one is bound: loads from an unbound slot
// return 0 but aren't free (~0.15-0.2 ms at 4K on an RTX 4080 SUPER, against ~0.02 ms for a bound mask).
#ifndef TAA_REACTIVE
#define TAA_REACTIVE (TAA_QUALITY >= 1)
#endif
// The clip box shrinks towards the pixel's own color by its mask times this. The min/max box of a thin lit feature
// (a laser beam) reaches its dark neighbors, so it kept the dark history of a light that just turned on; AMD FSR 3.1's
// box (Gaussian weighted mean and deviation, scaled 3 -> 1 by the mask) stays around the center. 0 disables.
#ifndef TAA_REACTIVE_BOX_SHRINK
#define TAA_REACTIVE_BOX_SHRINK 1.0
#endif
#ifndef TAA_REACTIVE_MAX
#define TAA_REACTIVE_MAX 0.9
#endif
// Shading change detection (AMD FSR 3.1's shading change pass), from Medium quality: content that changes without motion
// vectors (a shadow moving over static ground, particles without motion vectors or a reactive mask) otherwise keeps its old
// history inside the box that the flickering analysis widened. "shading_change_cs", dispatched before the resolve, compares
// this frame's luminance cross (5 taps) with the previous frame's around where this frame's sample was then: both sorted, the
// smallest signed relative difference between them, 0 where the jitter only reshuffles the same values. The differences and
// their signs are averaged over 2x2, 4x4 and 8x8 pixel blocks, and the change is the largest |mean difference * mean sign| of
// the three: aliasing changes sign from pixel to pixel and cancels out, a shading change has one sign over its area. As in FSR
// 3.1, the change (times "TAA_SHADING_CHANGE_SCALE", FSR's "fShadingChangeScale") narrows the box back to the unwidened one,
// takes the history weight toward 0 and shortens locks, each in proportion.
#ifndef TAA_SHADING_CHANGE
#define TAA_SHADING_CHANGE (TAA_QUALITY >= 1)
#endif
#ifndef TAA_SHADING_CHANGE_SCALE
#define TAA_SHADING_CHANGE_SCALE 1.0
#endif
// Changes up to this are taken for none, the rest rescaled to 0-1. Stochastic shading noise and static aliasing leave small
// values with a random sign in the 2x2 blocks (p99 0.04 under 20% per frame noise, 0.1 under 50%), against 0.4 and more for
// a lighting change, but the weight cap near 1 amplifies them: 0.014 cut the history from 24 to 17.5 samples, and a static
// textured railing (ME3LE) flickered at 7x DLAA's level. 0 disables.
#ifndef TAA_SHADING_CHANGE_THRESHOLD
#define TAA_SHADING_CHANGE_THRESHOLD 0.05
#endif
// FSR 3.1's luminance floor and smallest difference (the smallest normal half)
#define TAA_SHADING_CHANGE_EPSILON 6.10e-5

#define TAA_FLAG_RESET             (1u << 0)
#define TAA_FLAG_INVERTED_DEPTH    (1u << 1)
#define TAA_FLAG_REACTIVE_MASK     (1u << 2)

cbuffer LumaTAAData : register(b0)
{
   float2 RenderResolution;
   float2 InvRenderResolution;
   float2 MotionVectorScale;
   float2 DepthNearFar;
   uint Flags;
   float2 Jitter; // This frame's sample offset from the pixel center, in pixels
   float Padding;
   float2 OutputResolution; // Only used with "TAA_UPSCALE"
   float2 InvOutputResolution;
   float2 PreviousJitter; // The previous frame's "Jitter", for "shading_change_cs"
   float2 Padding2;
}

Texture2D<float3> SourceColor : register(t0);
Texture2D<float> DeviceDepth : register(t1);
Texture2D<float2> MotionVectors : register(t2);
Texture2D<float4> History : register(t3);
// Linear depth bits (asuint), written by "shading_change_cs" at Ultra quality and read by the Ultra quality resolve.
Texture2D<uint> ReconstructedPreviousDepth : register(t4);
// Alternating like the history, R32_UINT from Medium quality (R8_UINT at Low): the thin feature lock frames left or a moving
// pixel's speed in the low byte (see "TAA_LOCK_SPEED_STEPS"), the flickering analysis' darkest luminance average in the
// second byte (see "OutputFlicker" in "main"), and the 3x3's farthest linear depth as the top 16 bits of its float (see
// "TAA_PREVIOUS_DEPTH_CLIP")
Texture2D<uint> PreviousLock : register(t5);
// Flickering analysis state (R11G11B10_FLOAT, alternating like the history), see "OutputFlicker" in "main"
Texture2D<float3> PreviousFlicker : register(t6);
// Optional reactive mask (AMD FSR 2's): 1 where the color has no matching motion vectors (particles, VFX). See
// "TAA_REACTIVE".
Texture2D<float> ReactiveMask : register(t7);
// "TAA_SHADING_CHANGE": the previous frame's input luminance (R16_FLOAT, alternating like the history), read by
// "shading_change_cs", and its output, the 2x2, 4x4 and 8x8 block means of the differences (x) and their signs (y) as mips
// 0-2 of an R16G16_FLOAT texture covering the dispatch's 8x8 groups, read by the resolve
Texture2D<float> PreviousLuminance : register(t8);
Texture2D<float2> ShadingChangePyramid : register(t9);

RWTexture2D<float4> OutputHistory : register(u0);
RWTexture2D<float4> OutputColor : register(u1);
RWTexture2D<uint> OutputLock : register(u3);
RWTexture2D<float3> OutputFlicker : register(u4);
// "shading_change_cs" only (its own dispatch, so the slots overlap the resolve's)
RWTexture2D<float> OutputLuminance : register(u0);
RWTexture2D<float2> OutputShadingChange0 : register(u1);
RWTexture2D<float2> OutputShadingChange1 : register(u2);
RWTexture2D<float2> OutputShadingChange2 : register(u3);
RWTexture2D<uint> OutputReconstructedPreviousDepth : register(u4); // Ultra quality

SamplerState LinearClampSampler : register(s0);

float LinearizeDepth(float device_depth)
{
   const float near = DepthNearFar.x;
   const float far = DepthNearFar.y;
   if (Flags & TAA_FLAG_INVERTED_DEPTH)
   {
      return near * far / (near + device_depth * (far - near));
   }
   return near * far / (far - device_depth * (far - near));
}

// The 4 bilinear taps around "position" (pixels, texel centers at integer + 0.5): the top left texel and the
// fractional weights. Taps are "base + int2(tap & 1, tap >> 1)" with weight lerp(1 - f, f, offset) per axis.
void BilinearFootprint(float2 position, out int2 base, out float2 f)
{
   const float2 corner = position - 0.5;
   base = int2(floor(corner));
   f = corner - base;
}

// History weight cap for a displacement in history texels (see TAA_RESAMPLING_BLUR_TOLERANCE)
float ResamplingBlurCap(float2 displacement, float tolerance)
{
   const float2 fraction = frac(abs(displacement));
   const float resampling_blur = 0.5 * (fraction.x * (1.0 - fraction.x) + fraction.y * (1.0 - fraction.y));
   return tolerance / (tolerance + resampling_blur);
}

// Per axis weights of the 4 taps of a bicubic with sharpness "TAA_HISTORY_SHARPNESS" (a Keys cubic, a = -c), for a
// position "f" past the second tap (0-1)
void BicubicWeights(float2 f, out float2 w0, out float2 w1, out float2 w2, out float2 w3)
{
   const float c = TAA_HISTORY_SHARPNESS;
   w0 = f * (-c + f * (2.0 * c - c * f));
   w1 = 1.0 + f * f * (-(3.0 - c) + (2.0 - c) * f);
   w2 = f * (c + f * ((3.0 - 2.0 * c) - (2.0 - c) * f));
   w3 = f * f * (-c + c * f);
}

#if TAA_HISTORY_2X
// Jitter-aware Gaussian (sigma "TAA_HISTORY_2X_SIGMA" render pixels) of the 3x3 samples around "center" (render pixels),
// and its peak tap weight
float3 NeighborhoodGaussian(float3 neighborhood[9], int2 pixel, float2 center, out float peak)
{
   float3 sum = 0.0;
   float weight = 0.0;
   peak = 0.0;
   [unroll] for (int n = 0; n < 9; n++)
   {
      const float2 offset = pixel + float2(n % 3 - 1, n / 3 - 1) + 0.5 + Jitter - center;
      const float gauss = exp(-0.5 * dot(offset, offset) / (TAA_HISTORY_2X_SIGMA * TAA_HISTORY_2X_SIGMA));
      sum += neighborhood[n] * gauss;
      weight += gauss;
      peak = max(peak, gauss);
   }
   return sum / weight;
}
#endif

// Bilinear at Low quality, otherwise the 5-tap bicubic: MJP's 9-tap bilinear formulation without its 4 corner taps,
// renormalized (see the license notice at the top). "position" is in pixels, with texel centers at integer + 0.5.
float4 SampleHistory(float2 position, float2 inv_resolution)
{
#if TAA_QUALITY == 0
   return History.SampleLevel(LinearClampSampler, position * inv_resolution, 0);
#else
   const float2 texel_center = floor(position - 0.5) + 0.5;
   float2 w0, w1, w2, w3;
   BicubicWeights(position - texel_center, w0, w1, w2, w3);
   const float2 w12 = w1 + w2;
   const float2 uv0 = (texel_center - 1.0) * inv_resolution;
   const float2 uv3 = (texel_center + 2.0) * inv_resolution;
   const float2 uv12 = (texel_center + w2 / w12) * inv_resolution;
   float4 history = History.SampleLevel(LinearClampSampler, float2(uv12.x, uv0.y), 0) * (w12.x * w0.y);
   history += History.SampleLevel(LinearClampSampler, float2(uv0.x, uv12.y), 0) * (w0.x * w12.y);
   history += History.SampleLevel(LinearClampSampler, uv12, 0) * (w12.x * w12.y);
   history += History.SampleLevel(LinearClampSampler, float2(uv3.x, uv12.y), 0) * (w3.x * w12.y);
   history += History.SampleLevel(LinearClampSampler, float2(uv12.x, uv3.y), 0) * (w12.x * w3.y);
   // Without the corners the weights sum to 1 - (w0 + w3).x * (w0 + w3).y, 1.6% short at half a texel on both axes
   // (Catmull-Rom). Moving history loses that again every frame, in a pattern that follows the fractional position
   // (moire under a dolly) wherever the clip doesn't pull it back. The sum is 1 at whole texel offsets: static pixels
   // don't change.
   history /= 1.0 - (w0.x + w3.x) * (w0.y + w3.y);
   // The negative lobes can take the weight below 0 next to texels without history (upscaling)
   history.a = max(history.a, 0.0);
   return history;
#endif
}

// The game's color, its luminance and the resolved history as finite RGBA16F values, per component: NaN and infinities
// become 0 (as UE's MakeFinite and HDRP's NaN killer; an infinity clamped to FLT16_MAX would leave a firefly that decays
// over ~100 frames), finite values beyond the half range are clamped to it. Negative values are kept.
// The history would keep a NaN or an infinity forever (a lerp with a history weight of 0 still returns NaN, and an
// infinity times the bicubic's negative lobes becomes NaN), and its filtered reads spread it a pixel further every frame.
// "abs(x) <= FLT_MAX" is an ordered comparison: false for NaN, so fxc can't fold it as it does a bare "x == x". It avoids
// clamp(), as D3D min/max return the non-NaN operand (NaN would become -FLT16_MAX). For computed values and texture loads:
// a raw constant buffer read would need IsNaN_Strict (see Shaders/AGENTS.md).
float ToHalfRange(float value)
{
   return ((abs(value) <= FLT16_MAX) ? value : ((abs(value) <= FLT_MAX) ? (sign(value) * FLT16_MAX) : 0.0));
}
float3 ToHalfRange(float3 color)
{
   return ((abs(color) <= FLT16_MAX) ? color : ((abs(color) <= FLT_MAX) ? (sign(color) * FLT16_MAX) : 0.0));
}

// Moves "history" along the segment towards "target" until it is inside the box (Playdead "clip_aabb").
float3 ClipToBox(float3 history, float3 target, float3 box_min, float3 box_max)
{
   const float3 delta = history - target;
   const float3 limit = ((delta > 0.0) ? (box_max - target) : (box_min - target));
   const float3 scale = ((abs(delta) > 1e-8) ? saturate(limit / delta) : 1.0);
   return target + delta * min3(scale);
}

// The pixel of the 3x3 neighborhood (only its cross with "cross_only") with the closest device depth, that depth, and the
// farthest device depth
void FindClosestDepth(int2 pixel, int2 max_pixel, bool inverted_depth, bool cross_only, out float closest_depth,
                      out int2 closest_pixel, out float farthest_depth)
{
   closest_depth = (inverted_depth ? 0.0 : 1.0);
   closest_pixel = pixel;
   farthest_depth = (inverted_depth ? 1.0 : 0.0);
   // Four 2x2 gathers (the 4x4 texels around the 3x3) instead of 9 loads: one texture instruction per 4 texels. Quad q's top
   // left texel is at offset -1 + 2 * (q & 1, q >> 1); clamp addressing returns the same edge texels as the clamped loads
   // at the texture edge (the render edge on the top and left, even with a larger depth texture under render scale).
   // On a render sub-rect's right and bottom edge the gather reads texel max_pixel + 1, outside the render area.
   // The cross doesn't reach the bottom right quad.
   float depth_width, depth_height;
   DeviceDepth.GetDimensions(depth_width, depth_height);
   const float2 inv_depth_size = 1.0 / float2(depth_width, depth_height);
   float4 quads[4];
   [unroll] for (int quad = 0; quad < 4; quad++)
   {
      quads[quad] = ((cross_only && quad == 3) ? 0.0 : DeviceDepth.GatherRed(LinearClampSampler, (pixel + int2(quad & 1, quad >> 1) * 2) * inv_depth_size));
   }
   [unroll] for (int y = -1; y <= 1; y++)
   {
      [unroll] for (int x = -1; x <= 1; x++)
      {
         if (cross_only && x != 0 && y != 0)
         {
            continue;
         }
         const int2 sample_pixel = clamp(pixel + int2(x, y), 0, max_pixel);
         // Gather order: x bottom left, y bottom right, z top right, w top left
         const int2 offset = int2(x, y) + 1;
         const int2 in_quad = offset & 1;
         const float4 texels = quads[(offset.x >> 1) + (offset.y >> 1) * 2];
         const float depth = (in_quad.y ? (in_quad.x ? texels.y : texels.x) : (in_quad.x ? texels.z : texels.w));
         const bool closer = (inverted_depth ? (depth > closest_depth) : (depth < closest_depth));
         if (closer)
         {
            closest_depth = depth;
            closest_pixel = sample_pixel;
         }
         farthest_depth = (inverted_depth ? min(farthest_depth, depth) : max(farthest_depth, depth));
      }
   }
}

#if TAA_SHADING_CHANGE
// AMD FSR 3.1's sorting network for its 5 sample sets
void SortCross(inout float values[5])
{
   const uint2 pairs[9] = {uint2(0, 3), uint2(1, 4), uint2(0, 2), uint2(1, 3), uint2(0, 1), uint2(2, 4), uint2(1, 2), uint2(3, 4), uint2(2, 3)};
   [unroll] for (uint i = 0; i < 9; i++)
   {
      const float low = min(values[pairs[i].x], values[pairs[i].y]);
      values[pairs[i].y] = max(values[pairs[i].x], values[pairs[i].y]);
      values[pairs[i].x] = low;
   }
}

// The smallest signed relative difference (1 - min / max) between the values of two sorted luminance crosses, walked in
// order (AMD FSR 3.1's "ComputeMinimumDifference"); 0 when any two are equal (see "TAA_SHADING_CHANGE")
float SmallestCrossDifference(float current[5], float previous[5])
{
   SortCross(current);
   SortCross(previous);
   const float no_difference = 65504.0;
   float smallest = no_difference - 1.0;
   uint a = 0;
   uint b = 0;
   [unroll] for (uint i = 0; i < 5; i++)
   {
      const float difference = current[a] - previous[b];
      if (abs(difference) <= TAA_SHADING_CHANGE_EPSILON)
      {
         smallest = no_difference;
         break;
      }
      const float relative = sign(difference) * (1.0 - min(current[a], previous[b]) / max(current[a], previous[b]));
      smallest = ((abs(relative) < abs(smallest)) ? relative : smallest);
      // FSR's walk, which tests "b" against the already advanced "a"; its indices can pass the last value, so they stop there
      a = min(a + uint(current[a] < previous[b]), 4u);
      b = min(b + uint(current[a] >= previous[b]), 4u);
   }
   return ((smallest < no_difference - 1.0) ? smallest : 0.0);
}
#endif

// Threads per group side (Core dispatches ceil(size / 8) groups per axis, see "LumaTAA.hpp")
#define TAA_GROUP_SIZE   8
#define RECONSTRUCT_TILE 16
groupshared uint reconstruct_tile[RECONSTRUCT_TILE * RECONSTRUCT_TILE];

#if TAA_HISTORY_2X
// The 2x history texels of a group's 5x5 blocks (see "main"). Pixels that share the group center's motion span 19 texels
// per axis (2 per pixel, plus the block); the rest is slack for motion that differs within the group. A larger tile was
// no faster: it takes L1 cache away (groupshared memory and L1 share the same storage).
#define HISTORY_TILE 20
groupshared float4 history_tile[HISTORY_TILE * HISTORY_TILE];
// This frame's colors of the group's pixels and their 3x3 neighborhoods
#define COLOR_TILE (TAA_GROUP_SIZE + 2)
groupshared float3 color_tile[COLOR_TILE * COLOR_TILE];

// A 2x history texel, clamped to the texture: from the group's tile when the pixel's whole block is in it. A branch per
// texel ran faster on D3D11 than one branch around two copies of the block (fewer registers).
float4 LoadHistory2x(bool from_tile, int2 texel, int2 tile_origin, int2 max_texel)
{
   float4 value;
   [branch] if (from_tile)
   {
      const int2 tile_texel = texel - tile_origin;
      value = history_tile[tile_texel.y * HISTORY_TILE + tile_texel.x];
   }
   else
   {
      value = History.Load(int3(clamp(texel, 0, max_texel), 0));
   }
   return value;
}
#elif TAA_UPSCALE
// This frame's colors of the group's render pixels and their 3x3 neighborhoods: a group's 8x8 output pixels fall in at most 8
// render pixels per axis (9 with the rounding of their positions at a scale of ~1)
#define COLOR_TILE (TAA_GROUP_SIZE + 3)
groupshared float3 color_tile[COLOR_TILE * COLOR_TILE];
#endif

// clang-format parses a second "[numthreads] void f() {}" in a file as a lambda and indents it, hence the off region
// around this entry's end and the next one's signature.
#if TAA_SHADING_CHANGE
groupshared float2 shading_change_tile[TAA_GROUP_SIZE * TAA_GROUP_SIZE];

[numthreads(TAA_GROUP_SIZE, TAA_GROUP_SIZE, 1)] void shading_change_cs(uint3 dispatch_thread_id : SV_DispatchThreadID, uint3 group_thread_id : SV_GroupThreadID, uint3 group_id : SV_GroupID, uint group_index : SV_GroupIndex) {
   // AMD FSR 3.1's shading change and its pyramid (see "TAA_SHADING_CHANGE"), per render pixel. Every thread reaches the
   // barriers: threads past the edge repeat the edge pixel, as FSR's clamped loads do.
   const int2 max_pixel = int2(RenderResolution) - 1;
   const int2 pixel = min(int2(dispatch_thread_id.xy), max_pixel);
#if TAA_QUALITY >= 3
   // Ultra quality also reconstructs the previous depth (AMD FSR 2 "reconstruct previous depth"), sharing the closest 3x3
   // depth and its motion vector: every pixel's closest 3x3 depth is scattered along its motion vector into the previous
   // frame's bilinear footprint (taps weighing more than 1%), keeping the nearest.
   // "OutputReconstructedPreviousDepth" must be cleared to asuint(FLT_MAX) (0x7F7FFFFF) before the dispatch; positive
   // floats order like their bits, so InterlockedMin keeps the nearest linear depth.
   // The group's taps usually land in one small area (neighbors share motion): reduce them in groupshared memory
   // first, anchored around where the group's center pixel lands, and write one global atomic per touched texel.
   const int2 group_center = min(int2(group_id.xy) * TAA_GROUP_SIZE + TAA_GROUP_SIZE / 2, max_pixel);
   const float2 center_previous = group_center + 0.5 + MotionVectors.Load(int3(group_center, 0)) * MotionVectorScale;
   const int2 reconstruct_origin = int2(floor(center_previous)) - RECONSTRUCT_TILE / 2;
   [unroll] for (uint i = group_index; i < RECONSTRUCT_TILE * RECONSTRUCT_TILE; i += TAA_GROUP_SIZE * TAA_GROUP_SIZE)
   {
      reconstruct_tile[i] = asuint(FLT_MAX);
   }
#endif
   const int2 cross_offsets[5] = {int2(0, 0), int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};
   float current[5];
   [unroll] for (uint tap = 0; tap < 5; tap++)
   {
      current[tap] = max(ToHalfRange(dot(SourceColor.Load(int3(clamp(pixel + cross_offsets[tap], 0, max_pixel), 0)), Rec709_Luminance)), TAA_SHADING_CHANGE_EPSILON);
   }
   OutputLuminance[pixel] = current[0];

   float closest_depth;
   int2 closest_pixel;
   float farthest_depth;
   FindClosestDepth(pixel, max_pixel, (Flags & TAA_FLAG_INVERTED_DEPTH) != 0, false, closest_depth, closest_pixel, farthest_depth);
#if TAA_QUALITY >= 3
   GroupMemoryBarrierWithGroupSync(); // The reconstruction tile's clear
   // Threads past the edge repeat the edge pixel: scatter it once
   if (all(int2(dispatch_thread_id.xy) <= max_pixel))
   {
      const uint linear_depth_bits = asuint(LinearizeDepth(closest_depth));
      int2 footprint_base;
      float2 footprint_fraction;
      BilinearFootprint(pixel + 0.5 + MotionVectors.Load(int3(closest_pixel, 0)) * MotionVectorScale, footprint_base, footprint_fraction);
      [unroll] for (uint tap = 0; tap < 4; tap++)
      {
         const int2 offset = int2(tap & 1, tap >> 1);
         const float2 axis_weights = (offset ? footprint_fraction : (1.0 - footprint_fraction));
         const int2 tap_pixel = footprint_base + offset;
         if (axis_weights.x * axis_weights.y > 0.01 && all(tap_pixel >= 0) && all(tap_pixel <= max_pixel))
         {
            const int2 tile_pixel = tap_pixel - reconstruct_origin;
            if (all(tile_pixel >= 0) && all(tile_pixel < RECONSTRUCT_TILE))
            {
               InterlockedMin(reconstruct_tile[tile_pixel.y * RECONSTRUCT_TILE + tile_pixel.x], linear_depth_bits);
            }
            else
            {
               InterlockedMin(OutputReconstructedPreviousDepth[tap_pixel], linear_depth_bits);
            }
         }
      }
   }
#endif
   // The previous frame's sample nearest to where this frame's sample was then (both frames are jittered)
   const float2 previous_position = pixel + 0.5 + Jitter + MotionVectors.Load(int3(closest_pixel, 0)) * MotionVectorScale - PreviousJitter;
   float2 change = 0.0; // The difference and its sign
   if (!(Flags & TAA_FLAG_RESET) && all(previous_position >= 0.0) && all(previous_position < RenderResolution))
   {
      const int2 previous_pixel = int2(previous_position);
      // The cross from two gathers, at the corners above left and below right of its center. The luminance is at the render
      // resolution, so clamp addressing returns the same edge texels as clamped loads. Gather order: x bottom left, y bottom
      // right, z top right, w top left.
      const float2 previous_uv = previous_pixel * InvRenderResolution;
      const float4 top_left = PreviousLuminance.GatherRed(LinearClampSampler, previous_uv);
      const float4 bottom_right = PreviousLuminance.GatherRed(LinearClampSampler, previous_uv + InvRenderResolution);
      float previous[5] = {top_left.y, top_left.x, bottom_right.z, top_left.z, bottom_right.x}; // In "cross_offsets" order
      [unroll] for (uint previous_tap = 0; previous_tap < 5; previous_tap++)
      {
         previous[previous_tap] = max(previous[previous_tap], TAA_SHADING_CHANGE_EPSILON);
      }
      const float difference = SmallestCrossDifference(current, previous);
      change = float2(difference, sign(difference));
   }

   // The 2x2, 4x4 and 8x8 block means (FSR 3.1's mips 0-2), each from the 4 means of the level below, kept in the slot of
   // their top left pixel
   shading_change_tile[group_index] = change;
   GroupMemoryBarrierWithGroupSync();
#if TAA_QUALITY >= 3
   [unroll] for (uint j = group_index; j < RECONSTRUCT_TILE * RECONSTRUCT_TILE; j += TAA_GROUP_SIZE * TAA_GROUP_SIZE)
   {
      const uint value = reconstruct_tile[j];
      if (value != asuint(FLT_MAX))
      {
         InterlockedMin(OutputReconstructedPreviousDepth[reconstruct_origin + int2(j % RECONSTRUCT_TILE, j / RECONSTRUCT_TILE)], value);
      }
   }
#endif
   const uint2 local = group_thread_id.xy;
   [unroll] for (uint level = 0; level < 3; level++)
   {
      const uint step = 1u << level; // The level below's block size
      if (all((local & (2u * step - 1u)) == 0u))
      {
         const uint slot = local.y * TAA_GROUP_SIZE + local.x;
         const float2 mean = 0.25 * (shading_change_tile[slot] + shading_change_tile[slot + step] + shading_change_tile[slot + step * TAA_GROUP_SIZE] + shading_change_tile[slot + step * (TAA_GROUP_SIZE + 1)]);
         shading_change_tile[slot] = mean;
         const uint blocks_per_group = TAA_GROUP_SIZE / (2u * step);
         const int2 texel = int2(group_id.xy * blocks_per_group + local / (2u * step));
         if (level == 0)
         {
            OutputShadingChange0[texel] = mean;
         }
         else if (level == 1)
         {
            OutputShadingChange1[texel] = mean;
         }
         else
         {
            OutputShadingChange2[texel] = mean;
         }
      }
      GroupMemoryBarrierWithGroupSync();
   }
   // clang-format off
}
#endif

[numthreads(TAA_GROUP_SIZE, TAA_GROUP_SIZE, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID, uint3 group_id : SV_GroupID, uint group_index : SV_GroupIndex)
// clang-format on
{
#if TAA_UPSCALE
   // The group shares its colors, so every thread reaches the barrier: threads past the edge resolve the edge pixel again
   // and write the same values
   const int2 output_pixel = min(int2(dispatch_thread_id.xy), int2(OutputResolution) - 1);
   const float2 render_scale = OutputResolution * InvRenderResolution; // Output pixels per render pixel
   // The output pixel's center in render pixels, and the render pixel it falls in
   const float2 sample_center = (output_pixel + 0.5) / render_scale;
   const int2 pixel = min(int2(sample_center), int2(RenderResolution) - 1);
#elif TAA_HISTORY_2X
   // The group shares its history texels, so every thread reaches the barrier: threads past the edge resolve the edge
   // pixel again and write the same values
   const int2 pixel = min(int2(dispatch_thread_id.xy), int2(RenderResolution) - 1);
#else
   const int2 pixel = dispatch_thread_id.xy;
   if (any(pixel >= int2(RenderResolution)))
      return;
#endif

   const int2 max_pixel = int2(RenderResolution) - 1;
   const bool inverted_depth = (Flags & TAA_FLAG_INVERTED_DEPTH) != 0;

#if TAA_HISTORY_2X
   // The 5x5 blocks of the 2x history read below overlap, and their 25 loads per pixel go through the texture pipe that
   // bounds High and Ultra: the group loads their usual area once (one load per texel) into "history_tile", from where
   // the block of the group's center pixel starts (2 texels per pixel after its first pixel's), minus half the slack.
   const int2 max_texel = int2(RenderResolution) * 2 - 1;
   const int2 group_center = min(int2(group_id.xy) * TAA_GROUP_SIZE + TAA_GROUP_SIZE / 2, max_pixel);
   int2 center_block_base;
   float2 center_block_fraction;
   BilinearFootprint((group_center + 0.25 + MotionVectors.Load(int3(group_center, 0)) * MotionVectorScale) * 2.0, center_block_base, center_block_fraction);
   const int2 tile_origin = center_block_base - 1 - TAA_GROUP_SIZE - (HISTORY_TILE - 19) / 2;
   [unroll] for (uint tile_load = 0; tile_load * TAA_GROUP_SIZE * TAA_GROUP_SIZE < HISTORY_TILE * HISTORY_TILE; tile_load++)
   {
      const uint tile_index = group_index + tile_load * TAA_GROUP_SIZE * TAA_GROUP_SIZE;
      if (tile_index < HISTORY_TILE * HISTORY_TILE)
      {
         history_tile[tile_index] = History.Load(int3(clamp(tile_origin + int2(tile_index % HISTORY_TILE, tile_index / HISTORY_TILE), 0, max_texel), 0));
      }
   }
   // The same for this frame's 3x3 neighborhoods (9 loads per pixel), which the 2x resolve also reads again from the
   // tile rather than keeping them in registers through the history block
   const int2 group_origin = int2(group_id.xy) * TAA_GROUP_SIZE;
   [unroll] for (uint color_load = 0; color_load * TAA_GROUP_SIZE * TAA_GROUP_SIZE < COLOR_TILE * COLOR_TILE; color_load++)
   {
      const uint color_index = group_index + color_load * TAA_GROUP_SIZE * TAA_GROUP_SIZE;
      if (color_index < COLOR_TILE * COLOR_TILE)
      {
         float3 color = SourceColor.Load(int3(clamp(group_origin - 1 + int2(color_index % COLOR_TILE, color_index / COLOR_TILE), 0, max_pixel), 0));
         // One test of the whole color and the guard behind a branch: faster here than its select per channel
         [branch] if (!all(abs(color) <= FLT16_MAX))
         {
            color = ToHalfRange(color);
         }
         color_tile[color_index] = color;
      }
   }
   const int color_tile_center = (pixel.y - group_origin.y + 1) * COLOR_TILE + (pixel.x - group_origin.x + 1);
#elif TAA_UPSCALE
   // The 3x3 neighborhoods overlap more when upscaling (~2.25 output pixels per render pixel at 1.5x): the group loads its
   // render pixels once into "color_tile", from the render pixel of its first output pixel, minus the 3x3's border
   const int2 group_origin = min(int2((int2(group_id.xy) * TAA_GROUP_SIZE + 0.5) / render_scale), max_pixel) - 1;
   [unroll] for (uint color_load = 0; color_load * TAA_GROUP_SIZE * TAA_GROUP_SIZE < COLOR_TILE * COLOR_TILE; color_load++)
   {
      const uint color_index = group_index + color_load * TAA_GROUP_SIZE * TAA_GROUP_SIZE;
      if (color_index < COLOR_TILE * COLOR_TILE)
      {
         color_tile[color_index] = ToHalfRange(SourceColor.Load(int3(clamp(group_origin + int2(color_index % COLOR_TILE, color_index / COLOR_TILE), 0, max_pixel), 0)));
      }
   }
   const int color_tile_center = (pixel.y - group_origin.y) * COLOR_TILE + (pixel.x - group_origin.x);
#endif

   float closest_depth;
   int2 closest_pixel;
   float farthest_depth;
   // At Low quality only the cross is searched
   FindClosestDepth(pixel, max_pixel, inverted_depth, TAA_QUALITY == 0, closest_depth, closest_pixel, farthest_depth);
   const float2 motion = MotionVectors.Load(int3(closest_pixel, 0)) * MotionVectorScale;
   const float speed = length(motion);
   const float2 own_motion = MotionVectors.Load(int3(pixel, 0)) * MotionVectorScale; // "motion" may be a neighbor's
#if TAA_UPSCALE
   const float2 history_position = sample_center + motion;
#else
   const float2 history_position = pixel + 0.5 + motion;
#endif
   const bool offscreen = any(history_position < 0.0) || any(history_position >= RenderResolution);
   // The per pixel states (lock, flickering analysis) are reprojected with the nearest texel, at the output resolution
   // when upscaling
#if TAA_UPSCALE
   const int2 state_pixel = output_pixel;
   const float2 state_history_position = history_position * render_scale;
   const bool state_offscreen = any(state_history_position < 0.0) || any(state_history_position >= OutputResolution);
#else
   const int2 state_pixel = pixel;
   const float2 state_history_position = history_position;
   const bool state_offscreen = offscreen;
#endif
   const bool has_previous_state = !state_offscreen && !(Flags & TAA_FLAG_RESET);
   const int2 previous_pixel = int2(floor(state_history_position));
   const uint previous_lock_word = (((TAA_LOCK || TAA_FLICKER) && has_previous_state) ? PreviousLock.Load(int3(previous_pixel, 0)) : 0);
   const uint previous_lock_code = (TAA_LOCK ? (previous_lock_word & 0xFFu) : 0);

   // The depth clips run before the history read, so their taps don't keep the 2x history texels live (that made Ultra
   // 15-30% slower)
   const float current_linear_depth = LinearizeDepth(closest_depth);
   float depth_clip = 1.0;
#if TAA_QUALITY >= 3
   // Depth clip (AMD FSR 2): both depths come from this frame, so a camera moving along its view axis, which changes
   // every depth, can't make the test fail
   int2 footprint_base;
   float2 footprint_fraction;
   BilinearFootprint(history_position, footprint_base, footprint_fraction);
   float depth_clip_kept = 0.0;
   float depth_clip_weights = 0.0;
   [unroll] for (uint tap = 0; tap < 4; tap++)
   {
      const int2 offset = int2(tap & 1, tap >> 1);
      const float2 axis_weights = (offset ? footprint_fraction : (1.0 - footprint_fraction));
      const float weight = axis_weights.x * axis_weights.y;
      const int2 tap_pixel = footprint_base + offset;
      if (weight <= 0.01 || any(tap_pixel < 0) || any(tap_pixel > max_pixel))
      {
         continue;
      }
      const float separation = current_linear_depth - asfloat(ReconstructedPreviousDepth.Load(int3(tap_pixel, 0)));
      depth_clip_kept += weight * ((separation > 0.0) ? saturate(TAA_DEPTH_TOLERANCE * current_linear_depth / separation) : 1.0);
      depth_clip_weights += weight;
   }
   if (depth_clip_weights > 0.0)
   {
      depth_clip = depth_clip_kept / depth_clip_weights;
   }
#endif
#if TAA_PREVIOUS_DEPTH_CLIP
   // The previous frame's farthest 3x3 depth at the reprojected pixel, 0 without a state
   const float previous_farthest_depth = asfloat(previous_lock_word & 0xFFFF0000u);
   const float depth_separation = current_linear_depth - previous_farthest_depth;
   if (previous_farthest_depth > 0.0 && depth_separation > 0.0)
   {
      depth_clip = min(depth_clip, saturate(TAA_PREVIOUS_DEPTH_TOLERANCE * current_linear_depth / depth_separation));
   }
#endif

#if TAA_HISTORY_2X || TAA_UPSCALE
   GroupMemoryBarrierWithGroupSync(); // The tiles are loaded
#endif

   // The history is read before the flickering analysis, which compares its luminance to the range (see
   // "TAA_FLICKER_STEP_GROWTH")
#if TAA_UPSCALE
   const float4 history_sample = SampleHistory(state_history_position, InvOutputResolution);
   const float history_luminance = dot(history_sample.rgb, Rec709_Luminance);
#elif TAA_HISTORY_2X
   // The 4 history texels are 1 history texel apart, so they share their position past the taps and the bicubic weights:
   // each is an exact separable 4x4 bicubic of one 5x5 block of texels. "SampleHistory"'s 5 bilinear taps drop the 4
   // corner taps, an approximation that texels resampled every frame compound.
   int2 block_base;
   float2 block_fraction;
   BilinearFootprint((pixel + 0.25 + motion) * 2.0, block_base, block_fraction); // The top left texel's, in history texels
   const int2 block_origin = block_base - 1;
   float2 bicubic_weights[4];
   BicubicWeights(block_fraction, bicubic_weights[0], bicubic_weights[1], bicubic_weights[2], bicubic_weights[3]);
   const float dering = saturate(length(own_motion - motion) / TAA_HISTORY_2X_DERING_MOTION);
   // A block outside the tile (motion that differs within the group) is read from the texture
   const int2 tile_offset = block_origin - tile_origin;
   const bool in_tile = all(tile_offset >= 0) && all(tile_offset <= HISTORY_TILE - 5);
   float4 history_samples_2x[4];
   [unroll] for (uint block_texel = 0; block_texel < 4; block_texel++)
   {
      history_samples_2x[block_texel] = 0.0;
   }
   [unroll] for (int row = 0; row < 5; row++)
   {
      float4 row_texels[5];
      [unroll] for (int column = 0; column < 5; column++)
      {
         row_texels[column] = LoadHistory2x(in_tile, block_origin + int2(column, row), tile_origin, max_texel);
      }
      [unroll] for (uint row_texel = 0; row_texel < 4; row_texel++)
      {
         const int2 offset = int2(row_texel & 1, row_texel >> 1);
         const int tap = row - offset.y; // Of the texel's 4 rows
         if (tap < 0 || tap > 3)
         {
            continue;
         }
         float4 row_sum = 0.0;
         [unroll] for (int column_tap = 0; column_tap < 4; column_tap++)
         {
            row_sum += row_texels[offset.x + column_tap] * bicubic_weights[column_tap].x;
         }
         history_samples_2x[row_texel] += row_sum * bicubic_weights[tap].y;
      }
   }
   float3 history_mean = 0.0;
   [unroll] for (uint texel = 0; texel < 4; texel++)
   {
      // The deringing bounds are loaded again rather than tracked through the block, which kept 8 more vectors live there,
      // and only where there is deringing to do
      if (dering > 0.0)
      {
         const int2 nearest_origin = block_origin + 1 + int2(texel & 1, texel >> 1);
         float4 nearest_min = FLT_MAX;
         float4 nearest_max = -FLT_MAX;
         [unroll] for (uint nearest = 0; nearest < 4; nearest++)
         {
            const float4 nearest_texel = LoadHistory2x(in_tile, nearest_origin + int2(nearest & 1, nearest >> 1), tile_origin, max_texel);
            nearest_min = min(nearest_min, nearest_texel);
            nearest_max = max(nearest_max, nearest_texel);
         }
         history_samples_2x[texel] = lerp(history_samples_2x[texel], clamp(history_samples_2x[texel], nearest_min, nearest_max), dering);
      }
      // The negative lobes can take the weight below 0 next to texels without history
      history_samples_2x[texel].a = max(history_samples_2x[texel].a, 0.0);
      history_mean += history_samples_2x[texel].rgb * 0.25;
   }
   const float history_luminance = dot(history_mean, Rec709_Luminance);
#else
   const float4 history_sample = SampleHistory(history_position, InvRenderResolution);
   const float history_luminance = dot(history_sample.rgb, Rec709_Luminance);
#endif

   // 3x3 neighborhood color box (the 3x3 and the cross, averaged into a "rounded" box)
   float3 current = 0.0;
   float3 min_3x3 = FLT_MAX;
   float3 max_3x3 = -FLT_MAX;
   float3 sum_3x3 = 0.0;
   float3 min_cross = FLT_MAX;
   float3 max_cross = -FLT_MAX;
   float3 sum_cross = 0.0;
   float max_neighbor_luminance = 0.0; // For the thin feature lock
   // The flickering analysis uses the darkest sample's luminance. The luminance of the per channel min would be ~0 wherever
   // the samples have near-zero channels in different places, and hide lighting changes there.
   float min_luminance = FLT_MAX;
   float reactive = 0.0;
   float reactive_center = 0.0; // This pixel's own mask value (the 3x3 max is "reactive")
#if TAA_UPSCALE
   float3 neighborhood[9];
#endif
   [unroll] for (int y = -1; y <= 1; y++)
   {
      [unroll] for (int x = -1; x <= 1; x++)
      {
#if TAA_HISTORY_2X || TAA_UPSCALE
         const float3 color = color_tile[color_tile_center + y * COLOR_TILE + x];
#else
         const float3 color = ToHalfRange(SourceColor.Load(int3(clamp(pixel + int2(x, y), 0, max_pixel), 0)));
#endif
         const float luminance = dot(color, Rec709_Luminance);
         min_luminance = min(min_luminance, luminance);
#if TAA_LOCK
         if (x != 0 || y != 0)
         {
            max_neighbor_luminance = max(max_neighbor_luminance, luminance);
         }
#endif
         min_3x3 = min(min_3x3, color);
         max_3x3 = max(max_3x3, color);
         sum_3x3 += color;
         if (x == 0 || y == 0)
         {
            min_cross = min(min_cross, color);
            max_cross = max(max_cross, color);
            sum_cross += color;
         }
         if (x == 0 && y == 0)
         {
            current = color;
         }
#if TAA_UPSCALE
         neighborhood[(y + 1) * 3 + (x + 1)] = color;
#endif
      }
   }
#if TAA_REACTIVE
   [branch] if (Flags & TAA_FLAG_REACTIVE_MASK)
   {
      [unroll] for (int y = -1; y <= 1; y++)
      {
         [unroll] for (int x = -1; x <= 1; x++)
         {
            const float mask = ReactiveMask.Load(int3(clamp(pixel + int2(x, y), 0, max_pixel), 0));
            reactive = max(reactive, mask);
            if (x == 0 && y == 0)
            {
               reactive_center = mask;
            }
         }
      }
   }
#endif
   float shading_change = 0.0;
#if TAA_SHADING_CHANGE
   {
      // The pyramid's mips cover the 8x8 groups of "shading_change_cs": ceil(resolution / 8) * 8 render pixels per axis
#if TAA_UPSCALE
      const float2 pyramid_uv = sample_center / (ceil(RenderResolution / TAA_GROUP_SIZE) * TAA_GROUP_SIZE);
#else
      const float2 pyramid_uv = (pixel + 0.5) / (ceil(RenderResolution / TAA_GROUP_SIZE) * TAA_GROUP_SIZE);
#endif
      [unroll] for (uint mip = 0; mip < 3; mip++)
      {
         const float2 block = ShadingChangePyramid.SampleLevel(LinearClampSampler, pyramid_uv, mip);
         shading_change = max(shading_change, abs(block.x * block.y));
      }
      shading_change = saturate((shading_change * TAA_SHADING_CHANGE_SCALE - TAA_SHADING_CHANGE_THRESHOLD) / (1.0 - TAA_SHADING_CHANGE_THRESHOLD));
   }
#endif
   float3 box_min = 0.5 * (min_3x3 + min_cross);
   float3 box_max = 0.5 * (max_3x3 + max_cross);
   const float3 box_average = 0.5 * (sum_3x3 / 9.0 + sum_cross / 5.0);
   if (TAA_REACTIVE_BOX_SHRINK > 0.0)
   {
      box_min = lerp(box_min, current, saturate(reactive_center * TAA_REACTIVE_BOX_SHRINK));
      box_max = lerp(box_max, current, saturate(reactive_center * TAA_REACTIVE_BOX_SHRINK));
   }
   const float current_luminance = dot(current, Rec709_Luminance);
   const float mean_luminance = dot(sum_3x3, Rec709_Luminance) / 9.0; // For the flickering analysis' lighting test

   // Without the flickering analysis there's no lighting change test, so TAAU doesn't widen the static clip box
   bool lighting_changed_or_new = !TAA_FLICKER;
   uint min_luminance_code = 0; // Stored with the lock, see below
#if TAA_FLICKER
   {
      // x and y are the current sample luminance's range (at least 0), z and w the average luminances of the 3x3 darkest
      // sample and of the 3x3 mean (at least 2^-14, the smallest normal). x, y and w are in "PreviousFlicker" (R11G11B10:
      // 6, 6 and 5 bit mantissas, ~1.6% and 3% steps, see the store), z in the lock's high byte as a log2 code, 1 to 255
      // over 2^-14 to 2^18 (0, a cleared texture: no state). z only feeds the lighting test, a 10x change, so its 9% steps
      // are fine there; w's test is a 2x change.
      const uint previous_min_code = (previous_lock_word >> 8) & 0xFFu;
      const float3 previous_range_mean = (has_previous_state ? PreviousFlicker.Load(int3(previous_pixel, 0)) : 0.0);
      const float previous_min_average = ((previous_min_code > 0) ? exp2((previous_min_code - 1.0) * (32.0 / 254.0) - 14.0) : 0.0);
      const float4 previous_state = float4(previous_range_mean.xy, previous_min_average, previous_range_mean.z);
      const bool has_state = previous_min_code > 0;
      const float2 luminances = float2(min_luminance, mean_luminance);
      const float2 luminance_change = luminances - previous_state.zw;
      const float2 reset_change = float2(TAA_FLICKER_RESET_CHANGE, TAA_FLICKER_RESET_MEAN_CHANGE);
      const bool2 changed = abs(luminance_change) > reset_change * max(max(abs(luminances), abs(previous_state.zw)), 1e-4);
      const bool lighting_changed = TAA_FLICKER_RESET_CHANGE > 0.0 && all(changed) && luminance_change.x * luminance_change.y > 0.0;
      lighting_changed_or_new = lighting_changed || !has_state;
      const float previous_speed = ((previous_lock_code > TAA_LOCK_FRAMES) ? ((previous_lock_code - (TAA_LOCK_FRAMES + 1)) / TAA_LOCK_SPEED_STEPS) : 0.0);
      const bool highlight_history = history_luminance > TAA_LOCK_MIN_LUMINANCE && history_luminance > TAA_LOCK_RATIO * max(max_neighbor_luminance, current_luminance);
      const bool slowed = previous_speed - speed > TAA_FLICKER_MOTION_CHANGE && (previous_speed > TAA_FLICKER_DROP_SPEED || !highlight_history);
      const bool next_to_moving_highlight = speed > TAA_FLICKER_MOTION_CHANGE && length(own_motion) < TAA_LOCK_MAX_SPEED && length(own_motion - motion) > TAA_FLICKER_MOTION_CHANGE &&
                                            max_neighbor_luminance > TAA_LOCK_MIN_LUMINANCE && max_neighbor_luminance > TAA_LOCK_RATIO * current_luminance;
      const bool motion_changed = speed - previous_speed > TAA_FLICKER_MOTION_CHANGE || slowed || next_to_moving_highlight;
      const bool keeps_range = has_state && !lighting_changed && !motion_changed && reactive <= 0.0;
      float range_min = current_luminance;
      float range_max = current_luminance;
      if (keeps_range)
      {
         range_min = min(current_luminance, lerp(previous_state.x, current_luminance, TAA_FLICKER_ENVELOPE_DECAY));
         range_max = max(current_luminance, lerp(previous_state.y, current_luminance, TAA_FLICKER_ENVELOPE_DECAY));
      }
      const float2 luminance_average = max((has_state ? lerp(previous_state.zw, luminances, TAA_FLICKER_RESET_SMOOTHING) : luminances), 6.103515625e-5);
      float4 range_state = float4(max(range_min, 0.0), max(range_max, 0.0), luminance_average);
      float widening = TAA_FLICKER_GAMMA * (range_max - range_min);
#if !TAA_UPSCALE
      const float previous_range = previous_state.y - previous_state.x;
      const float step_margin = TAA_FLICKER_STEP_MARGIN * previous_range + 0.01 * abs(history_luminance);
      const bool history_in_range = abs(history_luminance - clamp(history_luminance, previous_state.x, previous_state.y)) <= step_margin;
      if (TAA_FLICKER_STEP_GROWTH > 0.0 && keeps_range && history_in_range)
      {
         widening = min(widening, TAA_FLICKER_GAMMA * TAA_FLICKER_STEP_GROWTH * previous_range);
         // Beyond twice the limit the range starts over, so the step doesn't widen the next frames either
         if (range_max - range_min > 2.0 * TAA_FLICKER_STEP_GROWTH * previous_range)
         {
            range_state.xy = max(current_luminance, 0.0);
         }
      }
#endif
      // D3D11 converts toward zero, which moves both range bounds down by up to a step. Rounded to the nearest (unbiased),
      // the 2x history's lab scores match the RGBA16F state's within 0.13% (rotation_fan's FLIP is 1.3-1.8% worse when
      // truncated); with the render resolution history the nearest made MV-less particles ghost 8% more, so it truncates.
      // Rounding up the min and down the max (a range that stops changing collapses) made static High and Ultra 15% slower.
#if TAA_HISTORY_2X
      OutputFlicker[state_pixel] = asfloat((asuint(range_state.xyw) + uint3(1u << 16, 1u << 16, 1u << 17)) & ~uint3(0x1FFFFu, 0x1FFFFu, 0x3FFFFu));
#else
      OutputFlicker[state_pixel] = range_state.xyw;
#endif
      min_luminance_code = 1 + uint(round((clamp(log2(range_state.z), -14.0, 18.0) + 14.0) * (254.0 / 32.0)));
      if (TAA_FLICKER_MAX_SPEED > 0.0)
      {
         widening *= saturate(1.0 - speed / TAA_FLICKER_MAX_SPEED);
      }
      widening *= 1.0 - shading_change;
      box_min -= widening;
      box_max += widening;
   }
#endif
   const float max_history_weight = TAA_MAX_HISTORY_WEIGHT * (1.0 - min(reactive, TAA_REACTIVE_MAX)) * (1.0 - shading_change);
   // Clipping into the current neighborhood also removes the bicubic's ringing.
   const float3 clip_target = clamp(box_average, box_min, box_max);

#if TAA_LOCK
   const bool lock_static = speed < TAA_LOCK_MAX_SPEED && reactive <= 0.0;
   uint lock_frames = 0;
   if (lock_static && current_luminance > TAA_LOCK_MIN_LUMINANCE && current_luminance > TAA_LOCK_RATIO * max_neighbor_luminance)
   {
      lock_frames = TAA_LOCK_FRAMES;
   }
   else if (previous_lock_code > 0 && previous_lock_code <= TAA_LOCK_FRAMES)
   {
      // A shading change shortens the lock in proportion (all of it at 1), as in AMD FSR 3.1
      lock_frames = uint(max(int(previous_lock_code) - 1 - int(round(shading_change * TAA_LOCK_FRAMES)), 0));
   }
   const bool locked = lock_static && lock_frames > 0;
#else
   const bool locked = false;
#endif

#if TAA_UPSCALE
   const float cap_fraction = min(ResamplingBlurCap(motion * render_scale, TAA_UPSCALE_BLUR_TOLERANCE), max_history_weight);
   const float sparse = TAA_UPSCALE_SPARSE * saturate((render_scale.x - TAA_UPSCALE_SPARSE_START) / (TAA_UPSCALE_SPARSE_FULL - TAA_UPSCALE_SPARSE_START));
   const float moving = saturate(length(motion * render_scale) / TAA_UPSCALE_STATIC_SPEED);
   const float sample_rate = render_scale.x * render_scale.y;
   const float cap_samples = cap_fraction / (1.0 - cap_fraction) / pow(abs(sample_rate), lerp(1.0, moving, sparse));
   const bool keep_box = lighting_changed_or_new || reactive > 0.0;
   const float static_box_scale = (keep_box ? 1.0 : lerp(lerp(1.0, TAA_UPSCALE_STATIC_BOX, sparse), 1.0, max(moving, shading_change)));
   const float3 box_center = 0.5 * (box_min + box_max);
   const float3 history_box_min = box_center - (box_center - box_min) * static_box_scale;
   const float3 history_box_max = box_center + (box_max - box_center) * static_box_scale;
   const float3 history = (locked ? history_sample.rgb : ClipToBox(history_sample.rgb, clip_target, history_box_min, history_box_max));
   const float lanczos_scale = TAA_UPSCALE_LANCZOS_SCALE * max(2.0 / render_scale.x, 1.0);
   float3 lanczos_sum = 0.0;
   float lanczos_weight = 0.0;
   float3 current_sum = 0.0;
   float current_weight = 0.0;
   float3 gauss_sum = 0.0;
   float gauss_weight = 0.0;
   float gauss_max = 0.0;
   [unroll] for (int n = 0; n < 9; n++)
   {
      // This sample's offset from the output pixel's center, in output pixels
      const float2 offset = (pixel + float2(n % 3 - 1, n / 3 - 1) + 0.5 + Jitter - sample_center) * render_scale;
      const float box_weight = (all(abs(offset) < 0.5) ? 1.0 : 0.0);
      current_sum += neighborhood[n] * box_weight;
      current_weight += box_weight;
      const float pi_x = PI * max(length(offset) * lanczos_scale, 1e-4);
      const float lanczos = ((pi_x < 2.0 * PI) ? (sin(pi_x) * sin(0.5 * pi_x) / (0.5 * pi_x * pi_x)) : 0.0);
      lanczos_sum += neighborhood[n] * lanczos;
      lanczos_weight += lanczos;
      const float gauss = exp(-0.5 * dot(offset, offset) / (TAA_UPSCALE_SIGMA * TAA_UPSCALE_SIGMA));
      gauss_sum += neighborhood[n] * gauss;
      gauss_weight += gauss;
      gauss_max = max(gauss_max, gauss);
   }
   const float3 fallback_output = gauss_sum / gauss_weight;
   const float fallback_weight = TAA_UPSCALE_FALLBACK_WEIGHT * gauss_max;
   const float3 box_output = ((current_weight > 0.0) ? (current_sum / current_weight) : fallback_output);
   const float box_sample_weight = ((current_weight > 0.0) ? min(current_weight, 1.0) : fallback_weight);
   const bool has_lanczos = lanczos_weight > 1e-3;
   const float3 lanczos_output = (has_lanczos ? clamp(lanczos_sum / lanczos_weight, box_min, box_max) : fallback_output);
   const float lanczos_sample_weight = (has_lanczos ? (lanczos_weight * lanczos_scale * lanczos_scale / 0.7395) : fallback_weight);
   const float3 current_output = lerp(box_output, lanczos_output, sparse);
   const float sample_weight = lerp(box_sample_weight, lanczos_sample_weight, sparse);
   const float history_samples = min(history_sample.a / max(1.0 - history_sample.a, 1e-4), cap_samples);
   const float history_validity = (has_previous_state ? depth_clip : 0.0);
   const float history_weight_output = history_samples * history_validity;
   // Rejected history (depth clip, offscreen) is replaced even in pixels that got no sample this frame
   const float blend = ((history_samples > 0.0) ? (history_samples / (history_samples + sample_weight)) : 0.0) * history_validity;
   const float3 resolved = ToHalfRange(lerp(current_output, history, blend));
   const float accumulated = history_weight_output + sample_weight;
   OutputHistory[output_pixel] = float4(resolved, accumulated / (accumulated + 1.0));
   OutputColor[output_pixel] = float4(resolved, 1.0);
   const bool any_history = history_weight_output > 0.0;
#elif TAA_HISTORY_2X
   const float cap_fraction = min(ResamplingBlurCap(motion * 2.0, TAA_HISTORY_2X_BLUR_TOLERANCE), max_history_weight);
   const float cap_samples = cap_fraction / (1.0 - cap_fraction) * TAA_HISTORY_2X_RATE;
   const float3 clipped_mean = ClipToBox(history_mean, clip_target, box_min, box_max);
   const float3 mean_delta = history_mean - clip_target;
   const float mean_clip_ratio = saturate(length(clipped_mean - clip_target) / max(length(mean_delta), 1e-8));
   const float mean_clip_scale = ((dot(abs(mean_delta), 1.0) > 1e-8) ? mean_clip_ratio : 1.0);
   float3 neighborhood[9];
   [unroll] for (int neighbor = 0; neighbor < 9; neighbor++)
   {
      neighborhood[neighbor] = color_tile[color_tile_center + (neighbor / 3 - 1) * COLOR_TILE + (neighbor % 3 - 1)];
   }
   // The masked pixel's Gaussian around its center (see "TAA_HISTORY_2X")
   float3 pixel_gauss = 0.0;
   [branch] if (reactive_center > 0.0)
   {
      float pixel_gauss_peak;
      pixel_gauss = NeighborhoodGaussian(neighborhood, pixel, pixel + 0.5, pixel_gauss_peak);
   }
   float3 output_sum = 0.0;
   bool any_history = false;
   float4 resolved_texels[4]; // Stored after the loop, past their guard
   [unroll] for (uint sub = 0; sub < 4; sub++)
   {
      const int2 sub_offset = int2(sub & 1, sub >> 1);
      const float2 center = pixel + 0.25 + 0.5 * sub_offset; // Render pixels
      const float2 history_center = center + motion;
      const bool offscreen_2x = any(history_center < 0.0) || any(history_center >= RenderResolution);
      const float4 history_sample_2x = history_samples_2x[sub];
      const float3 history_2x_clipped = clipped_mean + (history_sample_2x.rgb - history_mean) * mean_clip_scale;
      const float3 history_2x = (locked ? history_sample_2x.rgb : history_2x_clipped);
      float3 current_sum = 0.0;
      float current_weight = 0.0;
      [unroll] for (int n = 0; n < 9; n++)
      {
         const float2 offset = pixel + float2(n % 3 - 1, n / 3 - 1) + 0.5 + Jitter - center;
         const float box_weight = (all(abs(offset) < 0.25) ? 1.0 : 0.0);
         current_sum += neighborhood[n] * box_weight;
         current_weight += box_weight;
      }
      float gauss_max;
      const float3 gauss_estimate = NeighborhoodGaussian(neighborhood, pixel, center, gauss_max);
      const float3 texel_estimate = ((current_weight > 0.0) ? (current_sum / current_weight) : gauss_estimate);
      const float3 current_2x = lerp(texel_estimate, pixel_gauss, reactive_center);
      const float reactive_fallback_weight = reactive * TAA_HISTORY_2X_REACTIVE_FALLBACK * gauss_max;
      float sample_weight = ((current_weight > 0.0) ? min(current_weight, 1.0) : reactive_fallback_weight);
      if (TAA_HISTORY_2X_REACTIVE_SAMPLE)
      {
         sample_weight = max(sample_weight, reactive_center);
      }
      const float history_samples = min(history_sample_2x.a / max(1.0 - history_sample_2x.a, 1e-4), cap_samples);
      const float history_validity = ((offscreen_2x || (Flags & TAA_FLAG_RESET)) ? 0.0 : depth_clip);
      const float history_weight_2x = history_samples * history_validity;
      if (TAA_HISTORY_2X_DEFERRED > 0.0 && current_weight <= 0.0)
      {
         sample_weight = max(sample_weight, TAA_HISTORY_2X_DEFERRED * gauss_max * saturate(1.0 - history_weight_2x / TAA_HISTORY_2X_DEFERRED_SAMPLES));
      }
      // Rejected history (depth clip, offscreen) is replaced even in texels that got no sample this frame
      const float blend = ((history_samples > 0.0) ? (history_samples / (history_samples + sample_weight)) : 0.0) * history_validity;
      const float3 resolved_2x = lerp(current_2x, history_2x, blend);
      const float accumulated = history_weight_2x + sample_weight;
      resolved_texels[sub] = float4(resolved_2x, accumulated / (accumulated + 1.0));
      any_history = any_history || (history_weight_2x > 0.0);
   }
   // The guard of the 4 texels behind one branch on their half range test: faster than its select per channel
   [branch] if (!(all(abs(resolved_texels[0].rgb) <= FLT16_MAX) && all(abs(resolved_texels[1].rgb) <= FLT16_MAX) && all(abs(resolved_texels[2].rgb) <= FLT16_MAX) && all(abs(resolved_texels[3].rgb) <= FLT16_MAX)))
   {
      [unroll] for (uint fix = 0; fix < 4; fix++)
      {
         resolved_texels[fix].rgb = ToHalfRange(resolved_texels[fix].rgb);
      }
   }
   [unroll] for (uint store = 0; store < 4; store++)
   {
      OutputHistory[pixel * 2 + int2(store & 1, store >> 1)] = resolved_texels[store];
      output_sum += resolved_texels[store].rgb;
   }
   OutputColor[pixel] = float4(output_sum * 0.25, 1.0);
#else
   const float3 history = (locked ? history_sample.rgb : ClipToBox(history_sample.rgb, clip_target, box_min, box_max));
   const float history_weight_cap = min(ResamplingBlurCap(motion, TAA_RESAMPLING_BLUR_TOLERANCE), max_history_weight);
   const float history_weight = (has_previous_state ? (min(history_sample.a, history_weight_cap) * depth_clip) : 0.0);
   const float accumulated_weight = rcp(2.0 - history_weight);

#if TAA_REINHARD_BLEND
   // Sign preserving, so scRGB colors outside of BT.709 (negative channels) survive the round trip
   const float reinhard_peak = 1.0 / TAA_TONEMAP_SCALE;
   const float3 blended = lerp(Reinhard::ReinhardSimple(current, reinhard_peak), Reinhard::ReinhardSimple(history, reinhard_peak),
                               history_weight);
   const float3 resolved = ToHalfRange(Reinhard::InverseReinhardSimple(blended, reinhard_peak));
#else
   const float3 resolved = ToHalfRange(lerp(current, history, history_weight));
#endif

   OutputHistory[pixel] = float4(resolved, accumulated_weight);
   // Written separately so the output can be the game's own texture (any format), while the history stays RGBA16F.
   OutputColor[pixel] = float4(resolved, 1.0);
   const bool any_history = history_weight > 0.0;
#endif

#if TAA_LOCK || TAA_FLICKER
#if TAA_LOCK
   // A moving pixel holds no lock: its slot stores its speed for the flickering analysis (see "TAA_LOCK_SPEED_STEPS")
   const uint speed_code = (TAA_LOCK_FRAMES + 1) + uint(min(round(speed * TAA_LOCK_SPEED_STEPS), 254.0 - TAA_LOCK_FRAMES));
   const uint lock_code = ((speed >= TAA_LOCK_MAX_SPEED) ? speed_code : (any_history ? lock_frames : 0));
#else
   const uint lock_code = 0;
#endif
#if TAA_PREVIOUS_DEPTH_CLIP
   // Truncated to the float's top 16 bits: relative steps of 2^-7, far below the tolerance
   const uint farthest_depth_bits = asuint(LinearizeDepth(farthest_depth)) & 0xFFFF0000u;
#else
   const uint farthest_depth_bits = 0;
#endif
   OutputLock[state_pixel] = lock_code | (min_luminance_code << 8) | farthest_depth_bits;
#endif
}
