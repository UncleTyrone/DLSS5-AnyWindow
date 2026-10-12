// Experimental NVIDIA DLSS neural same-resolution filter. The native D3D12
// backend replaces this pass and supplies explicit zero motion/depth guides.

//!MAGPIE EFFECT
//!VERSION 4
//!SORT_NAME DLSSNR AI Filter (Experimental)

//!PARAMETER
//!LABEL Processing Resolution (%)
//!DEFAULT 100
//!MIN 25
//!MAX 100
//!STEP 1
int processingResolutionPercent;

//!PARAMETER
//!LABEL Style (0 Default, 1 Natural, 2 Cinematic)
//!DEFAULT 0
//!MIN 0
//!MAX 2
//!STEP 1
int style;

//!PARAMETER
//!LABEL Intensity (0 off, 1 normal, 2 strong)
//!DEFAULT 1
//!MIN 0
//!MAX 2
//!STEP 0.05
float intensity;

//!PARAMETER
//!LABEL Local Tone (0 off, 1 normal, 2 strong)
//!DEFAULT 1
//!MIN 0
//!MAX 2
//!STEP 0.05
float localToneStrength;

//!PARAMETER
//!LABEL Local Structure (0 soft, 1 normal, 2 sharp)
//!DEFAULT 1
//!MIN 0
//!MAX 2
//!STEP 0.05
float localStructureStrength;

//!PARAMETER
//!LABEL Skin (-1 follow, 0 off, 2 strong)
//!DEFAULT -1
//!MIN -1
//!MAX 2
//!STEP 0.05
float skinStructureStrength;

//!PARAMETER
//!LABEL Preset (0-3)
//!DEFAULT 0
//!MIN 0
//!MAX 3
//!STEP 1
int renderPreset;

//!PARAMETER
//!LABEL Automatic Mask
//!DEFAULT 0
//!MIN 0
//!MAX 1
//!STEP 1
int useAutoMask;

//!PARAMETER
//!LABEL Frame Guidance (0 Available, 1 Force Zero, 2 Motion Only, 3 Depth Only)
//!DEFAULT 0
//!MIN 0
//!MAX 3
//!STEP 1
int guidanceMode;

//!PARAMETER
//!LABEL Depth Interval
//!DEFAULT 4
//!MIN 1
//!MAX 8
//!STEP 1
int depthInferenceInterval;

//!PARAMETER
//!LABEL Passes (1-4)
//!DEFAULT 1
//!MIN 1
//!MAX 4
//!STEP 1
int passes;

//!PARAMETER
//!LABEL ViT Reuse (1 off, 2 every other frame)
//!DEFAULT 1
//!MIN 1
//!MAX 2
//!STEP 1
int vitReuseInterval;

//!PARAMETER
//!LABEL NVFP4 Hybrid (RTX 50)
//!DEFAULT 0
//!MIN 0
//!MAX 1
//!STEP 1
int modelPrecision;

//!PARAMETER
//!LABEL Detail Reuse (every other frame)
//!DEFAULT 0
//!MIN 0
//!MAX 1
//!STEP 1
int detailReuse;

//!PARAMETER
//!LABEL Feedback (0 blend, 1 full)
//!DEFAULT 1
//!MIN 0
//!MAX 1
//!STEP 0.05
float passFeedback;

//!PARAMETER
//!LABEL Style (-1 Inherit, 0 Default, 1 Natural, 2 Cinematic)
//!DEFAULT -1
//!MIN -1
//!MAX 2
//!STEP 1
int pass2Style;

//!PARAMETER
//!LABEL Intensity (-1 Inherit, 0 off, 1 normal, 2 strong)
//!DEFAULT -1
//!MIN -1
//!MAX 2
//!STEP 0.05
float pass2Intensity;

//!PARAMETER
//!LABEL Local Tone (-1 Inherit, 0 off, 1 normal, 2 strong)
//!DEFAULT 0
//!MIN -1
//!MAX 2
//!STEP 0.05
float pass2LocalTone;

//!PARAMETER
//!LABEL Local Structure (-1 Inherit, 0 soft, 1 normal, 2 sharp)
//!DEFAULT -1
//!MIN -1
//!MAX 2
//!STEP 0.05
float pass2LocalStructure;

//!PARAMETER
//!LABEL Skin (-1 Inherit, 0 off, 2 strong)
//!DEFAULT -1
//!MIN -1
//!MAX 2
//!STEP 0.05
float pass2Skin;

//!PARAMETER
//!LABEL Preset (-1 Inherit, 0-3)
//!DEFAULT -1
//!MIN -1
//!MAX 3
//!STEP 1
int pass2Preset;

//!PARAMETER
//!LABEL Style (-1 Inherit, 0 Default, 1 Natural, 2 Cinematic)
//!DEFAULT -1
//!MIN -1
//!MAX 2
//!STEP 1
int pass3Style;

//!PARAMETER
//!LABEL Intensity (-1 Inherit, 0 off, 1 normal, 2 strong)
//!DEFAULT -1
//!MIN -1
//!MAX 2
//!STEP 0.05
float pass3Intensity;

//!PARAMETER
//!LABEL Local Tone (-1 Inherit, 0 off, 1 normal, 2 strong)
//!DEFAULT 0
//!MIN -1
//!MAX 2
//!STEP 0.05
float pass3LocalTone;

//!PARAMETER
//!LABEL Local Structure (-1 Inherit, 0 soft, 1 normal, 2 sharp)
//!DEFAULT -1
//!MIN -1
//!MAX 2
//!STEP 0.05
float pass3LocalStructure;

//!PARAMETER
//!LABEL Skin (-1 Inherit, 0 off, 2 strong)
//!DEFAULT -1
//!MIN -1
//!MAX 2
//!STEP 0.05
float pass3Skin;

//!PARAMETER
//!LABEL Preset (-1 Inherit, 0-3)
//!DEFAULT -1
//!MIN -1
//!MAX 3
//!STEP 1
int pass3Preset;

//!PARAMETER
//!LABEL Style (-1 Inherit, 0 Default, 1 Natural, 2 Cinematic)
//!DEFAULT -1
//!MIN -1
//!MAX 2
//!STEP 1
int pass4Style;

//!PARAMETER
//!LABEL Intensity (-1 Inherit, 0 off, 1 normal, 2 strong)
//!DEFAULT -1
//!MIN -1
//!MAX 2
//!STEP 0.05
float pass4Intensity;

//!PARAMETER
//!LABEL Local Tone (-1 Inherit, 0 off, 1 normal, 2 strong)
//!DEFAULT 0
//!MIN -1
//!MAX 2
//!STEP 0.05
float pass4LocalTone;

//!PARAMETER
//!LABEL Local Structure (-1 Inherit, 0 soft, 1 normal, 2 sharp)
//!DEFAULT -1
//!MIN -1
//!MAX 2
//!STEP 0.05
float pass4LocalStructure;

//!PARAMETER
//!LABEL Skin (-1 Inherit, 0 off, 2 strong)
//!DEFAULT -1
//!MIN -1
//!MAX 2
//!STEP 0.05
float pass4Skin;

//!PARAMETER
//!LABEL Preset (-1 Inherit, 0-3)
//!DEFAULT -1
//!MIN -1
//!MAX 3
//!STEP 1
int pass4Preset;

//!PARAMETER
//!LABEL History Mode (0 Adaptive, 1 Static Stable, 2 Continuous)
//!DEFAULT 0
//!MIN 0
//!MAX 2
//!STEP 1
int historyMode;

//!TEXTURE
Texture2D INPUT;

//!TEXTURE
//!WIDTH INPUT_WIDTH
//!HEIGHT INPUT_HEIGHT
Texture2D OUTPUT;

//!SAMPLER
//!FILTER LINEAR
SamplerState sam;

//!PASS 1
//!STYLE PS
//!IN INPUT
//!OUT OUTPUT

MF4 Pass1(float2 pos) {
	return INPUT.SampleLevel(sam, pos, 0);
}
