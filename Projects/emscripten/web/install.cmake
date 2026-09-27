# Runs after linking the web build (cmake -DWEB=<this dir> -DOUT=<RunE> -P install.cmake).
#
# Puts the page shell's static files next to OpenFodder.html, adds index.html for plain static
# hosting, and stamps the service worker with a hash of the files it caches, so every new build
# replaces the cached copy of the last one.

foreach(Asset manifest.webmanifest icon-180.png icon-192.png icon-512.png icon-maskable-512.png)
    configure_file("${WEB}/${Asset}" "${OUT}/${Asset}" COPYONLY)
endforeach()

configure_file("${OUT}/OpenFodder.html" "${OUT}/index.html" COPYONLY)

set(Hashes "")
foreach(File OpenFodder.html OpenFodder.js OpenFodder.wasm OpenFodder.data)
    file(MD5 "${OUT}/${File}" Hash)
    string(APPEND Hashes "${Hash}")
endforeach()
string(MD5 Build "${Hashes}")
string(SUBSTRING "${Build}" 0 12 Build)

file(READ "${WEB}/sw.js" Worker)
string(REPLACE "@OPENFODDER_BUILD@" "${Build}" Worker "${Worker}")
file(WRITE "${OUT}/sw.js" "${Worker}")
