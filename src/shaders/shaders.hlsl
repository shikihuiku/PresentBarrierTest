struct VSInput
{
    float4 pos : POSITION;
    float4 color : COLOR;
};

struct PSInput
{
    float4 pos : SV_Position;
    float4 color : COLOR;
};

#if __SHADER_TARGET_STAGE == __SHADER_STAGE_VERTEX

PSInput VSMain(in VSInput input)
{
    PSInput result;
    result.pos = input.pos;
    result.color = input.color;
    return result;
}

#endif

#if __SHADER_TARGET_STAGE == __SHADER_STAGE_PIXEL

float4 PSMain(in PSInput input) : SV_Target0
{
    return input.color;
}

#endif
