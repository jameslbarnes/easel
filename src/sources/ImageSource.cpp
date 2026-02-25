#include "sources/ImageSource.h"

bool ImageSource::load(const std::string& path) {
    m_path = path;
    return m_texture.loadFromFile(path);
}
