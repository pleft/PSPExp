#ifdef GL_ES
precision mediump float;
#endif
uniform sampler2D uTex;
uniform int uDiscard;
uniform int uWhite;
varying vec2 vTex;
varying vec4 vColor;
vec4 shade(vec4 c) { return uWhite != 0 ? vec4(1.0) : c; }
void main()
{
    gl_FragColor = texture2D(uTex, vTex) * shade(vColor);
    if (uDiscard != 0 && gl_FragColor.a < 0.5)
        discard;
}
