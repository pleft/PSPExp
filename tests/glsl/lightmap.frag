uniform sampler2D uWall;
uniform sampler2D uLight;
uniform sampler2D uGlow;
uniform bool uUseGlow;
uniform float uBright;
void main()
{
    vec4 c = texture2D(uWall, gl_TexCoord[0].xy);
    if (c.a < 0.5)
        discard;
    c *= texture2D(uLight, gl_TexCoord[1].xy);
    c.rgb *= uBright;
    if (uUseGlow)
        c += texture2D(uGlow, gl_TexCoord[0].xy);
    c = clamp(c, 0.0, 1.0);
    float fog = exp(-gl_Fog.density * gl_Fog.density);
    c = mix(gl_Fog.color, c, clamp(fog, 0.0, 1.0));
    c.a = 1.0;
    gl_FragColor = c;
}
