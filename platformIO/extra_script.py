from os.path import join, isfile
import re
Import("env")
# access to global construction environment
ROOT_DIR = env['PROJECT_DIR']
# configuration file
configuration_file = join(ROOT_DIR, "esp3d", "configuration.h")
print("Check if need to add some library to path")
if isfile(configuration_file):
    fh = open(configuration_file, 'r', encoding='utf-8')
    entry = None
    for line in fh:
        pattern =r'^\s*#\s*define\s+SD_DEVICE\s+ESP_SDFAT2'
        entry = re.search(pattern, line)
        if entry:
            print("Need to add some SD FAT library to path")
            if (env["PIOPLATFORM"] == "espressif8266"):
                lib_ignore = env.GetProjectOption("lib_ignore")
                lib_ignore.append("SD(esp8266)")
                lib_ignore.append("SD")
                lib_ignore.append("SDFS")
                print("Ignore libs:", lib_ignore)
                env.GetProjectConfig().set(
                    "env:" + env["PIOENV"], "lib_ignore", lib_ignore)
                print("Add ESP8266SDFat2 library to path")
                env["LIBSOURCE_DIRS"].append(
                        "extra-libraries/ESP8266")
            else:
                print("Add SDFat2 library to path")
                env["LIBSOURCE_DIRS"].append("extra-libraries/ESP32")
            break
    fh.close()
    if entry is None:
        print("No need to add any extra library")
else:
    print("No configuration.h file found")
print(env["LIBSOURCE_DIRS"])

# ESP8266WebServer invokes hooks after stripping a URL query string. The raw
# upload endpoint must read X-Filename before the framework buffers the body,
# so retain the query string in the hook's URL argument.
raw_serial_enabled = False
if isfile(configuration_file):
    with open(configuration_file, "r", encoding="utf-8") as fh:
        raw_serial_enabled = any(re.match(
            r'^\s*#\s*define\s+COMMUNICATION_PROTOCOL\s+RAW_SERIAL\s*$',
            line) for line in fh)

if env["PIOPLATFORM"] == "espressif8266" and raw_serial_enabled:
    framework_dir = env.PioPlatform().get_package_dir(
        "framework-arduinoespressif8266")
    parsing_file = join(framework_dir, "libraries", "ESP8266WebServer",
                        "src", "Parsing-impl.h")
    if isfile(parsing_file):
        with open(parsing_file, "r", encoding="utf-8") as fh:
            parsing = fh.read()
        original = "auto whatNow = _hook(methodStr, url, &client, mime::getContentType);"
        patched = ("auto whatNow = _hook(methodStr, url + "
                   "(searchStr.length() ? \"?\" + searchStr : \"\"), "
                   "&client, mime::getContentType);")
        if original in parsing:
            with open(parsing_file, "w", encoding="utf-8") as fh:
                fh.write(parsing.replace(original, patched))
            print("Patched ESP8266WebServer hook to retain query strings")
        elif patched not in parsing:
            raise RuntimeError("Unsupported ESP8266WebServer hook implementation")
