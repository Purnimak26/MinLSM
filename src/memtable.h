#pragma once
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

enum Result { FOUND, DELETED, NOT_FOUND };

struct Entry {
    std::string key, value;
    bool deleted;
};

class MemTable {
private:
    struct Node {
        std::string key, value;
        bool deleted;
        std::vector<Node*> next;

        Node(const std::string& k, const std::string& v, bool d, int height)
            : key(k), value(v), deleted(d), next(height, nullptr) {}
    };

    static const int MAX_LEVEL = 16;
    Node* head;
    int level;

    int randomHeight() {
        int h = 1;
        while ((rand() & 1) && h < MAX_LEVEL) ++h;
        return h;
    }

    void set(const std::string& key, const std::string& value, bool deleted) {
        std::vector<Node*> update(MAX_LEVEL, head);
        Node* current = head;

        for (int i = level - 1; i >= 0; --i) {
            while (current->next[i] && current->next[i]->key < key)
                current = current->next[i];
            update[i] = current;
        }

        current = current->next[0];

        if (current && current->key == key) {      
            current->value = value;
            current->deleted = deleted;
            return;
        }

        int newHeight = randomHeight();
        if (newHeight > level) level = newHeight;  

        Node* newNode = new Node(key, value, deleted, newHeight);
        for (int i = 0; i < newHeight; ++i) {
            newNode->next[i] = update[i]->next[i];
            update[i]->next[i] = newNode;
        }
    }

public:
    MemTable() {
        head = new Node("", "", false, MAX_LEVEL);   
        level = 1;
    }

    MemTable(const MemTable&) = delete;
    MemTable& operator=(const MemTable&) = delete;

    ~MemTable() {
        Node* curr = head;
        while (curr) {
            Node* next = curr->next[0];
            delete curr;
            curr = next;
        }
    }

    void put(const std::string& key, const std::string& value) {
        set(key, value, false);
    }

    void del(const std::string& key) {
        set(key, "", true);
    }

    Result get(const std::string& key, std::string& out) const {
        Node* current = head;

        for (int i = level - 1; i >= 0; --i) {
            while (current->next[i] && current->next[i]->key < key)
                current = current->next[i];
        }

        current = current->next[0];
        if (!current || current->key != key) return NOT_FOUND;
        if (current->deleted) return DELETED;
        out = current->value;
        return FOUND;
    }

    std::vector<Entry> dump() const {
        std::vector<Entry> result;
        for (Node* c = head->next[0]; c; c = c->next[0])
            result.push_back({c->key, c->value, c->deleted});
        return result;
    }

   
};