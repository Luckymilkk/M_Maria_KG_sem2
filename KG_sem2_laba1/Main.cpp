#define STB_IMAGE_IMPLEMENTATION
#define TINYOBJLOADER_IMPLEMENTATION 
#define GLEW_STATIC
#include "stb_image.h"
#include "tiny_obj_loader.h"

#include <GL/glew.h> 
#include <GLFW/glfw3.h> 

#include <iostream>
#include <vector>
#include <string>
#include <map> // Нужен для хранения загруженных текстур

// --- Глобальные переменные ---
tinyobj::attrib_t attrib;
std::vector<tinyobj::shape_t> shapes;
std::vector<tinyobj::material_t> materials;

// Словарь: "Имя текстуры из файла .mtl" -> "ID текстуры в OpenGL"
std::map<std::string, GLuint> loadedTextures;

// Путь к папке, где лежит obj и текстуры 
const std::string base_dir = "models/source/";

// --- Функция загрузки одной текстуры ---
unsigned int loadTexture(const char* path)
{
    unsigned int textureID;
    glGenTextures(1, &textureID);

    int width, height, nrComponents;
    stbi_set_flip_vertically_on_load(true);

    unsigned char* data = stbi_load(path, &width, &height, &nrComponents, 0);
    if (data)
    {
        GLenum format;
        if (nrComponents == 1) format = GL_RED;
        else if (nrComponents == 3) format = GL_RGB;
        else if (nrComponents == 4) format = GL_RGBA;

        glBindTexture(GL_TEXTURE_2D, textureID);
        glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
        glGenerateMipmap(GL_TEXTURE_2D);

        // Для тайлинга  GL_REPEAT
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        stbi_image_free(data);
        std::cout << "Texture loaded: " << path << std::endl;
    }
    else
    {
        std::cout << "Texture failed to load at path: " << path << std::endl;
        stbi_image_free(data);
    }

    return textureID;
}

// --- Функция загрузки модели и материалов ---
void LoadModel(const std::string& filename) {
    std::string warn, err;

    // Передаем base_dir, чтобы загрузчик нашел .mtl файл
    bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, filename.c_str(), base_dir.c_str());

    if (!warn.empty()) std::cout << "Warn: " << warn << std::endl;
    if (!err.empty()) std::cerr << "Err: " << err << std::endl;
    if (!ret) exit(1);

    // ПРОХОДИМ ПО МАТЕРИАЛАМ И ГРУЗИМ ТЕКСТУРЫ
    for (const auto& mat : materials) {
        if (!mat.diffuse_texname.empty()) {
            // Если текстура еще не загружена
            if (loadedTextures.find(mat.diffuse_texname) == loadedTextures.end()) {
                std::string fullPath = base_dir + mat.diffuse_texname; // Полный путь
                GLuint texID = loadTexture(fullPath.c_str());
                loadedTextures[mat.diffuse_texname] = texID;
            }
        }
    }
}

// --- Анимация UV ---
void ApplyTextureTransform(float u, float v, float time) {
    float tilingScale = 3.0f; // Тайлинг (повторение)
    float animSpeed = 0.2f;   // Скорость

    // Сдвигаем U координату от времени для анимации
    float u_final = u * tilingScale + (time * animSpeed);
    float v_final = v * tilingScale;

    glTexCoord2f(u_final, v_final);
}

// --- Отрисовка ---
void DrawModel(float time) {
    for (size_t s = 0; s < shapes.size(); s++) {
        size_t index_offset = 0;

        for (size_t f = 0; f < shapes[s].mesh.num_face_vertices.size(); f++) {
            int fv = shapes[s].mesh.num_face_vertices[f];

            // --- УЧЕТ МАТЕРИАЛОВ ---
            int matId = shapes[s].mesh.material_ids[f];
            if (matId >= 0) {
                // Цвет материала
                glColor3fv(materials[matId].diffuse);
                // Текстура материала
                std::string texName = materials[matId].diffuse_texname;
                if (!texName.empty() && loadedTextures.count(texName)) {
                    glEnable(GL_TEXTURE_2D);
                    glBindTexture(GL_TEXTURE_2D, loadedTextures[texName]);
                }
                else {
                    glBindTexture(GL_TEXTURE_2D, 0); // Нет текстуры
                }
            }

            glBegin(GL_POLYGON);
            for (size_t v = 0; v < fv; v++) {
                tinyobj::index_t idx = shapes[s].mesh.indices[index_offset + v];

                if (idx.normal_index >= 0) {
                    glNormal3f(attrib.normals[3 * idx.normal_index + 0],
                        attrib.normals[3 * idx.normal_index + 1],
                        attrib.normals[3 * idx.normal_index + 2]);
                }

                if (idx.texcoord_index >= 0) {
                    float tx = attrib.texcoords[2 * idx.texcoord_index + 0];
                    float ty = attrib.texcoords[2 * idx.texcoord_index + 1];

                    // Применяем тайлинг и анимацию (Задание 3)
                    ApplyTextureTransform(tx, ty, time);
                }

                glVertex3f(attrib.vertices[3 * idx.vertex_index + 0],
                    attrib.vertices[3 * idx.vertex_index + 1],
                    attrib.vertices[3 * idx.vertex_index + 2]);
            }
            glEnd();
            index_offset += fv;
        }
    }
}

// --- MAIN ---
int main(void)
{
    // 1. Инициализация GLFW
    if (!glfwInit()) return -1;

    GLFWwindow* window = glfwCreateWindow(800, 600, "Homework OBJ & Texture", NULL, NULL);
    if (!window) {
        glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(window);

    // 2. Инициализация GLEW
    if (glewInit() != GLEW_OK) std::cout << "GLEW Init Error!" << std::endl;

    // Настройки OpenGL
    glEnable(GL_DEPTH_TEST); // Чтобы полигоны не перекрывали друг друга неправильно

    // Включаем простой свет, чтобы видеть материалы (иначе все будет плоским)
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    // Свет падает с позиции (1, 1, 1)
    GLfloat light_pos[] = { 1.0, 1.0, 1.0, 0.0 };
    glLightfv(GL_LIGHT0, GL_POSITION, light_pos);
    // Включаем автоматическую нормализацию нормалей (важно при скейлинге)
    glEnable(GL_NORMALIZE);
    // Материалы управляют цветом
    glEnable(GL_COLOR_MATERIAL);

    // 3. Загрузка модели (Укажи правильный путь!)
    std::string modelPath = base_dir + "725b3a4da0ef_Tiny_green_starw__3.obj"; // Например, cube.obj или teapot.obj
    LoadModel(modelPath);

    // 4. Главный цикл
    while (!glfwWindowShouldClose(window))
    {
        // Очистка экрана
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Настройка камеры
        int width, height;
        glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);
        float ratio = width / (float)height;

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        // Простая перспектива
        float fov = 60.0f * 3.14159f / 180.0f;
        float nearP = 0.1f;
        float farP = 100.0f;
        float f = 1.0f / tan(fov / 2);
        float mat[16] = {
            f / ratio, 0, 0, 0,
            0, f, 0, 0,
            0, 0, (farP + nearP) / (nearP - farP), -1,
            0, 0, (2 * farP * nearP) / (nearP - farP), 0
        };
        glLoadMatrixf(mat);

        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        // Отодвигаем камеру назад и немного поворачиваем
        glTranslatef(0.0f, -1.0f, -5.0f);
        glRotatef(20, 1.0f, 0.0f, 0.0f);
        // Вращаем модель со временем
        glRotatef((float)glfwGetTime() * 10.0f, 0.0f, 1.0f, 0.0f);

        // Отрисовка
        DrawModel((float)glfwGetTime());

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glfwTerminate();
    return 0;
}
