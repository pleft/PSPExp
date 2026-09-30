uniform vec4 uLight;
uniform vec4 uDiffuse;
uniform vec4 uAmbient;
uniform sampler2D uBase;
uniform samplerCube uSky;
varying vec2 vUV;
varying vec3 vN;
varying vec3 vRay;
void main()
{
    vec4 base = texture2D(uBase, vUV);
    if (base.a < 0.5)
        discard;
    vec4 sky = textureCube(uSky, vRay);
    vec4 shade = vec4(vec3(max(0.0, dot(vN, uLight.xyz))), 1.0) * uDiffuse + uAmbient;
    shade.rgb = clamp(shade.rgb, 0.0, 1.0);
    shade.a = 1.0;
    gl_FragColor = base * shade + vec4(sky.rgb, 0.0);
}
