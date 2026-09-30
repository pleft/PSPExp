// a reflective surface: lit, a cube map along the eye vector reflected about the normal
uniform mat4 uMVP;
uniform mat4 uMV;
uniform mat3 uNormal;
uniform mat4 uEnvRot;
attribute vec3 aPos;
attribute vec3 aNorm;
attribute vec2 aUV;
varying vec2 vUV;
varying vec3 vN;
varying vec3 vRay;
void main()
{
    vUV = aUV;
    vN = uNormal * aNorm;
    gl_Position = uMVP * vec4(aPos, 1.0);
    vec3 eye = vec3(uMV * vec4(aPos, 1.0));
    vRay = normalize(mat3(uEnvRot) * reflect(eye, vN));
}
