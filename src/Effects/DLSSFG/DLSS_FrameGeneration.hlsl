// Experimental DLSS Frame Generation adapter. Native NGX D3D12 code replaces
// this pass and inserts generated frames before the captured real frame.

//!MAGPIE EFFECT
//!VERSION 4
//!SORT_NAME DLSS FG_Experimental (No Smooth Motion)

//!PARAMETER
//!LABEL Frame Multiplier
//!DEFAULT 2
//!MIN 2
//!MAX 6
//!STEP 1
float multiplier;

//!PARAMETER
//!LABEL Use Motion Vectors
//!DEFAULT 1
//!MIN 0
//!MAX 1
//!STEP 1
float useMotionVectors;

//!PARAMETER
//!LABEL Use Estimated Depth (Experimental)
//!DEFAULT 0
//!MIN 0
//!MAX 1
//!STEP 1
float useEstimatedDepth;

//!TEXTURE
Texture2D INPUT;

//!TEXTURE
//!WIDTH INPUT_WIDTH
//!HEIGHT INPUT_HEIGHT
Texture2D OUTPUT;

//!SAMPLER
//!FILTER POINT
SamplerState sam;

//!PASS 1
//!STYLE PS
//!IN INPUT
//!OUT OUTPUT

MF4 Pass1(float2 pos) {
	return INPUT.SampleLevel(sam, pos, 0);
}
