// glass: the sky only, where the base texture is clear
uniform sampler2D uBase;
uniform samplerCube uSky;
uniform int uPattern;   // a screen-door pattern, for seeing through
varying vec2 vUV;
varying vec3 vN;
varying vec3 vRay;
void main()
{
    if (uPattern != 0 && mod(floor(gl_FragCoord.x) + floor(gl_FragCoord.y), 2.0) < 0.5)
        discard;
    vec4 base = texture2D(uBase, vUV);
    if (base.a >= 0.5)
        discard;
    gl_FragColor = textureCube(uSky, vRay);
}
