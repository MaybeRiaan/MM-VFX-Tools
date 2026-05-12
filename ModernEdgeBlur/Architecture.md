# Project: Modern Edge Blur (After Effects C++ Plugin)
# Base Template: SDK Skeleton Sample

## 1. Objective
A professional-grade edge blurring tool that detects high-contrast transitions in the Alpha channel and applies a weighted blur to soften jagged edges or "mattes" without blurring the interior of the image.

## 2. Parameter IDs (ModernEdgeBlur.h)
- EDGE_BLUR_INPUT (0)
- EDGE_BLUR_EDGE_RADIUS: The "thickness" of the edge detection (Float).
- EDGE_BLUR_EDGE_OFFSET: Shift the detection inward or outward (Float).
- EDGE_BLUR_BLUR_AMOUNT: Strength of the blur (Float).
- EDGE_BLUR_BLUR_TYPE: 0: Box, 1: Gaussian (Popup).
- EDGE_BLUR_SUB_PIXEL: Enable high-quality sampling (Checkbox).
- EDGE_BLUR_DILATE_EN: Enable color dilation to fix dark edges (Checkbox).
- EDGE_BLUR_DILATE_AMT: Radius of dilation (Float).
- EDGE_BLUR_NUM_PARAMS

## 3. Core Logic Flow (ModernEdgeBlur.cpp)

### Step 1: The Edge Matte (Alpha Detection)
Detect the "boundary" of the object.
- Identify pixels where Alpha is between 1 and 254.
- Create a grayscale "Edge Weight" map where 1.0 is the center of the edge and 0.0 is the solid interior/exterior.

### Step 2: The Blur (Two-Pass Convolution)
- Perform a Horizontal pass, then a Vertical pass.
- The Blur Amount must be multiplied by the "Edge Weight" from Step 1.
- This ensures only the edges get softened, leaving the center tack-sharp.

### Step 3: Color Dilation (The "Un-Halacer")
- If DILATE_EN is true: For every edge pixel, look at the surrounding fully-opaque pixels and "pull" their color into the semi-transparent edge.
- This prevents the "black fringe" common in standard blurs.

## 4. Implementation Guidelines
- Support 8-bit (PF_Pixel8) and 16-bit (PF_Pixel16) paths.
- Thread-safe logic for Multi-Frame Rendering (MFR).
- Use AE SDK macros for parameter checkout.