#pragma once

#include <string>
#include <vector>

// Every product QEngine can build an instance as, for the New Device sheet
// and for naming an instance's product in the list. The same set as the
// README's device tables.
struct Product {
    std::string code;
    std::string name;    // as the maker writes it
    std::string family;  // the sheet's grouping: Engine, MPC, HeadRush, Alesis
    std::string arch;    // armv7 or arm64
    // The file name prefixes this product's own update images use, so a
    // release can be read from a name like PRIME41.6.0BETA2.
    std::vector<std::string> images;
    bool montero = false;  // an Engine product, which MONTERO can load into
};

const std::vector<Product>& catalog();
const Product* findProduct(const std::string& code);

// The release a firmware file holds, read from its name: "5.3.0", or
// "5.1.0 beta 8". Empty when the name carries none.
std::string firmwareVersion(const std::string& filename);

// An instance name in the repo's convention, product code and release:
// jp24-5.3.0, jp22-5.1.0b8.
std::string defaultInstanceName(const std::string& code, const std::string& firmware);
