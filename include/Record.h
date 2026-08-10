#ifndef RECORD_H
#define RECORD_H

#include <string>

// Represents a single row/entity in the "accounts" table.
struct Record {
    int id;
    std::string name;
    double balance;

    Record() : id(0), name(""), balance(0.0) {}
    Record(int id, const std::string& name, double balance)
        : id(id), name(name), balance(balance) {}

    bool operator==(const Record& other) const {
        return id == other.id && name == other.name && balance == other.balance;
    }
};

#endif // RECORD_H
