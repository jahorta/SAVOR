#pragma once
#include <string>
#include "Utils/IniDoc.h"

class GuiLayoutStore {
public:
    static void LoadLayout(IniDoc& doc, std::string& out_ini_text);
    static void SaveLayout(IniDoc& doc, const std::string& ini_text);
};
