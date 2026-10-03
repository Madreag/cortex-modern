// Small symbol fixture. The tests read its PE/PDB files; they never execute it.
extern "C" __declspec(dllexport) int SDL_main() { return 7; }
extern "C" __declspec(dllexport) int WinMain() { return SDL_main(); }
extern "C" __declspec(dllexport) int main() { return WinMain(); }
