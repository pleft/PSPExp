// a lightmapped wall: the texture's and the lightmap's coordinates, the old built-ins
attribute vec4 aPos;
attribute vec2 aUV;
attribute vec2 aLM;
void main()
{
    gl_TexCoord[0] = vec4(aUV, 0.0, 0.0);
    gl_TexCoord[1] = vec4(aLM, 0.0, 0.0);
    gl_Position = gl_ModelViewProjectionMatrix * aPos;
}
