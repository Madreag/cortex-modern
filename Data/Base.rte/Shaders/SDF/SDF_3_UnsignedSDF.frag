#version 330 core
in vec2 textureUV;
out vec4 FragColor;

uniform sampler2D uSampler;
uniform vec2 uViewSize;
uniform float uMaxDist;
uniform vec2 uWrapsXY;  // x/y = 1 if scene wraps on that axis.

void main() {
    vec4 n = texture(uSampler, textureUV);
    if (n.x < 0.0) {
        // No seed in view (treat as far away)
        FragColor = vec4(1.0,1.0,1.0,1.0); // White = far
        return;
    }
    vec2 nearestPx = n.xy * uViewSize;
    vec2 fragPx = gl_FragCoord.xy;

    vec2 d = abs(nearestPx - fragPx);
	// Toroidal wrap only on axes the scene actually wraps on.
	if (uWrapsXY.x > 0.5) d.x = min(d.x, uViewSize.x - d.x);
	if (uWrapsXY.y > 0.5) d.y = min(d.y, uViewSize.y - d.y);

	float dist = length(d);

    // Normalize for display (0-1)
    float outVal = clamp(dist / uMaxDist, 0.0, 1.0);	
	FragColor = vec4(outVal, 0.0, 0.0, 1.0);
}