// a helper that returns from both sides of an if (Quake's models light themselves so)
uniform vec3 uLightDir;
attribute vec4 aPos;
attribute vec3 aNormal;
float shade(vec3 n)
{
    float d = dot(n, uLightDir);
    if (d < 0.0)
        return 1.0 + d * 0.25;
    else
        return 1.0 + d;
}
void main()
{
    gl_Position = gl_ModelViewProjectionMatrix * aPos;
    gl_FrontColor = vec4(vec3(shade(aNormal) * 0.5), 1.0);
}
