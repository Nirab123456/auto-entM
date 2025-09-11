git clone https://github.com/drogonframework/drogon.git
cd drogon
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j
sudo make install
sudo ldconfig      # refresh linker's cache if installing to /usr/local/lib






# from project-root
rm -rf build
mkdir build && cd build
cmake ..
make -j$(nproc)
