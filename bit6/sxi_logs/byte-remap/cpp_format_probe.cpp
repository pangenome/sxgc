#include "sxi_format.hpp"
#include <iostream>
int main(int argc,char**argv){try{if(argc!=2)return 2;sxi::Container c(argv[1]);return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
