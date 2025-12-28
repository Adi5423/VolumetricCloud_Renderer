#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <iostream>
#include <string>

// ---------------- SHADERS ----------------

const char* kVertexShader = R"(#version 460 core
const vec2 verts[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
);
out vec2 vUV;
void main()
{
    gl_Position = vec4(verts[gl_VertexID], 0.0, 1.0);
    vUV = gl_Position.xy * 0.5 + 0.5;
}
)";

const char* kFragmentShader = R"(#version 460 core
in vec2 vUV;
out vec4 FragColor;

uniform vec3 uCameraPos;
uniform mat4 uInvViewProj;
uniform float uTime;

// --------- Cloud Volume Bounds ---------
bool insideCloudVolume(vec3 p)
{
    vec3 minB = vec3(-200.0, 20.0, -200.0);
    vec3 maxB = vec3( 200.0, 120.0, 200.0);
    return all(greaterThanEqual(p, minB)) &&
           all(lessThanEqual(p, maxB));
}


// --------- Hash / Noise ---------
float hash(vec3 p)
{
    p = fract(p * 0.3183099 + vec3(0.1));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float noise(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);

    return mix(
        mix(mix(hash(i + vec3(0,0,0)), hash(i + vec3(1,0,0)), f.x),
            mix(hash(i + vec3(0,1,0)), hash(i + vec3(1,1,0)), f.x), f.y),
        mix(mix(hash(i + vec3(0,0,1)), hash(i + vec3(1,0,1)), f.x),
            mix(hash(i + vec3(0,1,1)), hash(i + vec3(1,1,1)), f.x), f.y),
        f.z
    );
}

// --------- Density Field ---------
float sampleDensity(vec3 p)
{
    // --- Cloud layer heights (match your volume bounds)
    float bottom = 20.0;
    float top    = 120.0;

    // Normalize height inside layer
    float h = clamp((p.y - bottom) / (top - bottom), 0.0, 1.0);

    // UE-style vertical profile
    float heightProfile =
          smoothstep(0.0, 0.15, h) *
          smoothstep(1.0, 0.75, h);

    // Wind
    vec3 wind = vec3(uTime * 0.04, 0.0, uTime * 0.02);

    // --- Base shape (large billows)
    float base = noise(p * 0.02 + wind);

    // --- Detail noise (adds 3D depth)
    float detail = noise(p * 0.08 + wind * 1.5);

    // --- Erosion (carves edges)
    float erosion = noise(p * 0.15 - wind);

    // Combine like UE
    float density = base;
    density = mix(density, detail, 0.35);
    density -= erosion * 0.25;

    // Coverage & remap
    density = smoothstep(0.45, 0.75, density);

    // Apply vertical shaping
    density *= heightProfile;

    return density;
}


// --------- Sky Color --------- hekper function*
vec3 skyColor(vec3 dir)
{
    float t = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 horizon = vec3(0.75, 0.85, 1.0);
    vec3 zenith  = vec3(0.35, 0.55, 0.9);
    return mix(horizon, zenith, t);
}


// --------- Raymarch ---------
void main()
{
    // Reconstruct view ray
    vec2 uv = vUV * 2.0 - 1.0;
    vec4 clip = vec4(uv, -1.0, 1.0);
    vec4 world = uInvViewProj * clip;
    world.xyz /= world.w;
    vec3 rayDir = normalize(world.xyz - uCameraPos);

    float t = 0.0;
    float tMax = 250.0;

    vec3 color = vec3(0.0);
    float transmittance = 1.0;

    const int STEPS = 112;
    float stepSize = tMax / float(STEPS);
    float stepMul = mix(1.0, 2.5, t / tMax);
    float adaptiveStep = stepSize * stepMul;

    for (int i = 0; i < STEPS; i++)
    {
        if (transmittance < 0.01)
            break;

        vec3 pos = uCameraPos + rayDir * t;
        float distFade = smoothstep(0.0, 1.0, t / tMax);
        float density = 0.0;
        /*if (insideCloudVolume(pos))
        {
            FragColor = vec4(1.0, 0.0, 0.0, 1.0);
            return;
        }*/

        if (insideCloudVolume(pos))
        {
            density = sampleDensity(pos) * distFade;
        }
        
        

        if (density > 0.0001)
        {
            float absorb = density * 0.9;
            // Isotropic base (view-independent)
            float baseLight = 0.6;
            
            // Mild forward scattering (VERY subtle)
            float forward = pow(max(dot(rayDir, normalize(vec3(0.2, 0.7, 0.5))), 0.0), 2.0);
            float phase = mix(1.0, 1.25, forward);
            
            // Final stable lighting
            vec3 scatter = vec3(1.0) * absorb * baseLight * phase;

            color += scatter * transmittance;
            transmittance *= exp(-absorb * 1.0);
        }

        t += adaptiveStep;;
        
        if (density < 0.0005)
        {
            t += adaptiveStep * 1.0; // skip faster in empty space
            continue;
        }
    }

    vec3 sky = skyColor(rayDir);
    vec3 cloudColor = color * vec3(0.95, 0.97, 1.0);
    vec3 finalColor = cloudColor + sky * transmittance;
    FragColor = vec4(finalColor, 1.0);

}
)";

// ---------------- UTILS ----------------

GLuint Compile(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);

    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[2048];
        glGetShaderInfoLog(s, 2048, nullptr, log);
        std::cerr << log << "\n";
    }
    return s;
}

GLuint CreateProgram()
{
    GLuint vs = Compile(GL_VERTEX_SHADER, kVertexShader);
    GLuint fs = Compile(GL_FRAGMENT_SHADER, kFragmentShader);

    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);

    glDeleteShader(vs);
    glDeleteShader(fs);
    return p;
}

// ---------------- MAIN ----------------

int main()
{
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "Volumetric Raymarch", nullptr, nullptr);
    glfwMakeContextCurrent(window);
    gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);

    GLuint program = CreateProgram();

    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    glDisable(GL_DEPTH_TEST);

    // Camera / input state
    glm::vec3 camPos = glm::vec3(0.0f, 2.0f, -250.0f);
    glm::vec3 worldUp = glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 camFront = glm::normalize(glm::vec3(0.0f) - camPos);
    float yaw = glm::degrees(atan2(camFront.z, camFront.x));
    float pitch = glm::degrees(asin(camFront.y));

    double lastX = 640.0, lastY = 360.0;
    bool firstMouse = true;
    bool rightMouseDownPrev = false;

    float baseSpeed = 40.0f; // units per second
    float sprintMultiplier = 4.0f;
    float mouseSensitivity = 0.1f; // degrees per pixel

    double lastFrame = glfwGetTime();

    while (!glfwWindowShouldClose(window))
    {
        double currentFrame = glfwGetTime();
        float deltaTime = (float)(currentFrame - lastFrame);
        lastFrame = currentFrame;

        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClear(GL_COLOR_BUFFER_BIT);

        float time = (float)currentFrame;

        // Input: right mouse hold for look + movement
        bool rightDown = (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);

        // Handle cursor mode on press/release
        if (rightDown && !rightMouseDownPrev)
        {
            // just pressed
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            firstMouse = true; // avoid jump
        }
        else if (!rightDown && rightMouseDownPrev)
        {
            // just released
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
        rightMouseDownPrev = rightDown;

        if (rightDown)
        {
            double xpos, ypos;
            glfwGetCursorPos(window, &xpos, &ypos);
            if (firstMouse)
            {
                lastX = xpos;
                lastY = ypos;
                firstMouse = false;
            }

            double xoffset = xpos - lastX;
            double yoffset = lastY - ypos; // reversed: moving up should increase pitch
            lastX = xpos;
            lastY = ypos;

            yaw += (float)(xoffset * mouseSensitivity);
            pitch += (float)(yoffset * mouseSensitivity);

            if (pitch > 89.0f) pitch = 89.0f;
            if (pitch < -89.0f) pitch = -89.0f;

            // update front vector from yaw/pitch
            glm::vec3 front;
            float yawRad = glm::radians(yaw);
            float pitchRad = glm::radians(pitch);
            front.x = cos(yawRad) * cos(pitchRad);
            front.y = sin(pitchRad);
            front.z = sin(yawRad) * cos(pitchRad);
            camFront = glm::normalize(front);

            // Movement while holding right mouse
            float velocity = baseSpeed * (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? sprintMultiplier : 1.0f) * deltaTime;

            if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
                camPos += camFront * velocity;
            if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
                camPos -= camFront * velocity;
            glm::vec3 camRight = glm::normalize(glm::cross(camFront, worldUp));
            if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
                camPos -= camRight * velocity;
            if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
                camPos += camRight * velocity;
            if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS)
                camPos -= worldUp * velocity; // Q = up
            if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS)
                camPos += worldUp * velocity; // E = down
        }

        glm::mat4 proj = glm::perspective(glm::radians(60.0f), float(w) / float(h), 0.1f, 200.0f);
        glm::mat4 view = glm::lookAt(camPos, camPos + camFront, worldUp);
        glm::mat4 invVP = glm::inverse(proj * view);

        glUseProgram(program);
        glUniform3fv(glGetUniformLocation(program, "uCameraPos"), 1, glm::value_ptr(camPos));
        glUniformMatrix4fv(glGetUniformLocation(program, "uInvViewProj"), 1, GL_FALSE, glm::value_ptr(invVP));
        glUniform1f(glGetUniformLocation(program, "uTime"), time);

        glDrawArrays(GL_TRIANGLES, 0, 3);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
    glfwTerminate();
    return 0;
}