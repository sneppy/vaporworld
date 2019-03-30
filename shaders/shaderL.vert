#version 430 core

layout(location=0) in vec4 pos;
layout(location=1) in vec4 normal;
layout(location=2) in vec4 color;

uniform mat4 modelMatrix;
uniform mat4 viewMatrix;

uniform vec3 cameraLocation;
uniform vec4 timeColor;
const float PI = 3.1415926535897932384626433832795;

out vec4 vColor;

void main(){

    vec3 FragPos = vec3(modelMatrix * pos);

    float z = cameraLocation[2];
    float f = 0.05*sin(z/20)+0.025*cos(2*z/20)+0.03*sin(3*z/20);
    float d = FragPos[2] - cameraLocation[2];
    FragPos.y += f*pow(d,2);
    
    float x = timeColor.x;
    float r = 0.5;//0.5+0.5*sin(10*x);
    float g = 0.0;
    float b = 0.75;//0.5-0.5*sin(10*x);
    vColor = vec4(r,g,b,exp(-0.2*(abs(d)-4)));

    gl_Position = viewMatrix * vec4(FragPos, 1.f);
}