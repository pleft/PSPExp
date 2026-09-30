// two poses of a model blended by a weight, as Quake animates its models
uniform float uBlend;
attribute vec4 aPose1;
attribute vec4 aPose2;
void main()
{
    vec4 v = mix(vec4(aPose1.xyz, 1.0), vec4(aPose2.xyz, 1.0), uBlend);
    gl_Position = gl_ModelViewProjectionMatrix * v;
}
