#pragma once
#include <vector>
#include <string>

template <typename T>
class NodeEx {
public:
    std::string name;
    std::vector<NodeEx> children;
    T data;
};

class Node {
public:
    std::string name;
    std::vector<Node> children;
};

class TreeView {
public:
    static void Draw(const Node* n);

    template <typename T>
    static void DrawEx(const NodeEx<T>* n);
};