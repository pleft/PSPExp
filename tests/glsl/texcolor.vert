uniform mat4 uMvp;
attribute vec3 aPos;
attribute vec4 aColor;
attribute vec2 aTex;
varying vec2 vTex;
varying vec4 vColor;
void main()
{
    vTex = aTex;
    vColor = aColor;
    gl_Position = uMvp * vec4(aPos, 1.0);
}
