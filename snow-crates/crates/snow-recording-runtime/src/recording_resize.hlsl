struct AxisSpan {
    uint first;
    uint count;
    float firstWeight;
    float lastWeight;
};

cbuffer ResizeConstants : register(b0) {
    uint2 sourceSize;
    uint2 outputSize;
    int2 cropOrigin;
    uint rotation;
    float interiorWeight;
    int2 destinationOrigin;
    uint2 dispatchSize;
};

Texture2D<float4> source : register(t0);
StructuredBuffer<AxisSpan> spans : register(t1);
RWTexture2D<float4> output : register(u0);

float weight(AxisSpan span, uint tap) {
    return tap == 0 ? span.firstWeight
        : tap + 1 == span.count ? span.lastWeight : interiorWeight;
}

int2 nativePosition(uint2 logical) {
    int2 position = int2(logical);
    if (rotation == 1) {
        position = int2(logical.y, sourceSize.x - 1 - logical.x);
    } else if (rotation == 2) {
        position = int2(sourceSize - 1 - logical);
    } else if (rotation == 3) {
        position = int2(sourceSize.y - 1 - logical.y, logical.x);
    }
    return cropOrigin + position;
}

// The first pass reads only covered pixels inside the explicit source crop.
// RGBA16F intermediate storage preserves fractional coverage until final output.
[numthreads(16, 16, 1)]
void horizontal(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= outputSize)) {
        return;
    }
    AxisSpan span = spans[id.x];
    float3 color = 0;
    for (uint tap = 0; tap < span.count; ++tap) {
        int2 position = nativePosition(uint2(span.first + tap, id.y));
        color += source.Load(int3(position, 0)).rgb * weight(span, tap);
    }
    output[id.xy] = float4(color, 1);
}

[numthreads(16, 16, 1)]
void vertical(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= outputSize)) {
        return;
    }
    AxisSpan span = spans[id.y];
    float3 color = 0;
    for (uint tap = 0; tap < span.count; ++tap) {
        color += source.Load(int3(id.x, span.first + tap, 0)).rgb * weight(span, tap);
    }
    output[id.xy] = float4(color, 1);
}

// Exact native RGB placement also avoids the video processor's crop-phase
// interpolation. The destination can be clipped against the recording region.
[numthreads(16, 16, 1)]
void copyCrop(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= dispatchSize)) {
        return;
    }
    int2 destination = destinationOrigin + int2(id.xy);
    if (any(destination < 0) || any(destination >= int2(outputSize))) {
        return;
    }
    output[destination] = float4(source.Load(int3(nativePosition(id.xy), 0)).rgb, 1);
}
