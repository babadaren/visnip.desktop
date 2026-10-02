cmake_minimum_required(VERSION 3.25)

project(Visnip
    VERSION 0.4.1
    DESCRIPTION "Visnip lightweight screenshot and pin tool"
    LANGUAGES CXX
)

if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

option(VISNIP_STRICT_WARNINGS "Treat warnings as errors" OFF)
# The open-source build translates only on this computer. Online modes (cloud,
# intranet, hybrid) and a default service address are opt-in at build time, so
# no service endpoint is compiled into the published sources or binaries.
option(VISNIP_ONLINE_TRANSLATION "Offer cloud, intranet and hybrid translation modes" OFF)
set(VISNIP_DEFAULT_SERVICE_URL "" CACHE STRING
    "Default translation service root for online modes; empty means the user enters one")
option(VISNIP_BUILD_TOOLS "Build repository maintenance tools" OFF)

include(CTest)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Concurrent Gui Widgets Svg Network)
if(BUILD_TESTING)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Test)
endif()

if(MINGW)
    add_compile_options(-Wa,-mbig-obj)
endif()

set(VISNIP_CORE_SOURCES
    src/core/AnnotationModel.cpp
    src/core/AppConfig.cpp
    src/core/DesignTokens.cpp
    src/core/LocalTranslation.cpp
    src/core/LongCaptureMatch.cpp
    src/core/OcrLanguagePack.cpp
    src/core/OcrPostProcess.cpp
    src/core/OfflineResourceCatalog.cpp
    src/core/OutputFilenamePattern.cpp
    src/core/PerfLog.cpp
    src/core/QuestionAnswer.cpp
    src/core/TranslationCompositor.cpp
    src/core/TranslationLanguage.cpp
)

# The question panel is also compiled straight into its tests, which then do
# not need the rest of the UI library (OCR runtime, capture overlay).
set(VISNIP_QUESTION_SOURCES
    src/services/QuestionAnswerService.cpp
    src/ui/question/QuestionPanel.cpp
)

set(VISNIP_UI_SOURCES
    ${VISNIP_QUESTION_SOURCES}
    src/app/AppController.cpp
    src/platform/HotkeyManager.cpp
    src/platform/WindowCaptureExclusion.cpp
    src/services/FastTranslationDiagnostics.cpp
    src/services/ImageTranslationService.cpp
    src/services/LocalTextTranslationService.cpp
    src/services/OfflineTranslationService.cpp
    src/services/OfflineResourceService.cpp
    src/services/ImageOutputService.cpp
    src/services/OcrPackDownloadService.cpp
    src/services/OcrService.cpp
    src/services/TextTranslationService.cpp
    src/ui/IconUtils.cpp
    src/ui/capture/CaptureOverlayWindow.cpp
    src/ui/pin/PinManager.cpp
    src/ui/pin/PinWindow.cpp
    src/ui/settings/SettingsDialog.cpp
    src/ui/widgets/CompactToolbar.cpp
)

add_library(visnip_core STATIC ${VISNIP_CORE_SOURCES})
target_include_directories(visnip_core PUBLIC src)
target_link_libraries(visnip_core PUBLIC Qt6::Core Qt6::Gui)

target_compile_definitions(visnip_core PUBLIC
    VISNIP_VERSION="${PROJECT_VERSION}"
    VISNIP_ONLINE_TRANSLATION=$<BOOL:${VISNIP_ONLINE_TRANSLATION}>
    VISNIP_DEFAULT_SERVICE_URL="${VISNIP_DEFAULT_SERVICE_URL}"
)

add_library(visnip_ui STATIC ${VISNIP_UI_SOURCES})
target_include_directories(visnip_ui PUBLIC src)
target_include_directories(visnip_ui PRIVATE third_party/onnxruntime/include)
target_link_libraries(visnip_ui PUBLIC visnip_core Qt6::Core Qt6::Concurrent Qt6::Gui Qt6::Widgets Qt6::Svg Qt6::Network)

if(WIN32)
    target_link_libraries(visnip_core PUBLIC bcrypt crypt32)
    target_link_libraries(visnip_ui PUBLIC user32 gdi32 dwmapi)
endif()

function(visnip_enable_warnings)
    if(MSVC)
        set(warning_options /W4)
        if(VISNIP_STRICT_WARNINGS)
            list(APPEND warning_options /WX)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        set(warning_options -Wall -Wextra -Wpedantic)
        if(VISNIP_STRICT_WARNINGS)
            list(APPEND warning_options -Werror)
        endif()
    else()
        return()
    endif()

    # Apply warnings only to maintained sources. Qt-generated MOC translation
    # units can emit diagnostics from Qt headers that application code cannot fix.
    foreach(source IN LISTS ARGN)
        set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_OPTIONS ${warning_options})
    endforeach()
endfunction()

visnip_enable_warnings(
    ${VISNIP_CORE_SOURCES}
    ${VISNIP_UI_SOURCES}
    src/main.cpp
)

add_executable(visnip
    src/main.cpp
    resources/visnip_assets.qrc
)

if(WIN32)
    target_sources(visnip PRIVATE resources/visnip.rc)
endif()

target_link_libraries(visnip PRIVATE visnip_ui)

if(WIN32)
    set_target_properties(visnip PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# Fast-translate OCR assets are fetched by scripts/fetch_ocr_assets.ps1.
# Runtime and detector are shared; each recognition pack is staged separately
# so one missing optional pack cannot disable all local OCR.
set(VISNIP_OCR_SHARED_FILES
    onnxruntime.dll
    onnxruntime_providers_shared.dll
    ch_PP-OCRv4_det_infer.onnx
)
set(VISNIP_OCR_SHARED_SOURCES
    ${CMAKE_SOURCE_DIR}/third_party/onnxruntime/bin/onnxruntime.dll
    ${CMAKE_SOURCE_DIR}/third_party/onnxruntime/bin/onnxruntime_providers_shared.dll
    ${CMAKE_SOURCE_DIR}/third_party/ocr/ch_PP-OCRv4_det_infer.onnx
)
set(VISNIP_OCR_SHARED_READY TRUE)
foreach(asset IN LISTS VISNIP_OCR_SHARED_SOURCES)
    if(NOT EXISTS ${asset})
        set(VISNIP_OCR_SHARED_READY FALSE)
    endif()
endforeach()
if(VISNIP_OCR_SHARED_READY)
    add_custom_command(TARGET visnip POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E remove_directory $<TARGET_FILE_DIR:visnip>/ocr
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:visnip>/ocr
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${VISNIP_OCR_SHARED_SOURCES} $<TARGET_FILE_DIR:visnip>/ocr
        COMMENT "Staging shared OCR runtime and detector"
    )
    foreach(pack_id IN ITEMS general-v5 korean-v5 latin-v5 cyrillic-v5 arabic-v5)
        set(pack_source ${CMAKE_SOURCE_DIR}/third_party/ocr/rec/${pack_id})
        if(EXISTS ${pack_source}/model.onnx)
            add_custom_command(TARGET visnip POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:visnip>/ocr/rec/${pack_id}
                COMMAND ${CMAKE_COMMAND} -E copy_directory ${pack_source} $<TARGET_FILE_DIR:visnip>/ocr/rec/${pack_id}
                COMMENT "Staging OCR recognition pack ${pack_id}"
            )
        endif()
    endforeach()
    # Migration-only fallback for installations fetched before PP-OCRv5.
    if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/ocr/ch_PP-OCRv4_rec_infer.onnx
       AND EXISTS ${CMAKE_SOURCE_DIR}/third_party/ocr/ppocr_keys_v1.txt)
        add_custom_command(TARGET visnip POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_SOURCE_DIR}/third_party/ocr/ch_PP-OCRv4_rec_infer.onnx
                ${CMAKE_SOURCE_DIR}/third_party/ocr/ppocr_keys_v1.txt
                $<TARGET_FILE_DIR:visnip>/ocr
        )
    endif()
else()
    message(WARNING "Shared OCR assets missing; run scripts/fetch_ocr_assets.ps1.")
endif()

if(BUILD_TESTING)
    add_executable(visnip_core_tests
        tests/tst_core.cpp
    )
    target_link_libraries(visnip_core_tests PRIVATE visnip_core Qt6::Test)
    visnip_enable_warnings(tests/tst_core.cpp)
    add_test(NAME visnip_core_tests COMMAND visnip_core_tests)

    add_executable(visnip_ui_tests
        tests/tst_ui.cpp
        resources/visnip_assets.qrc
    )
    target_link_libraries(visnip_ui_tests PRIVATE visnip_ui Qt6::Test)
    visnip_enable_warnings(tests/tst_ui.cpp)
    add_test(NAME visnip_ui_tests COMMAND visnip_ui_tests)
    add_executable(visnip_question_tests
        tests/tst_question.cpp
        ${VISNIP_QUESTION_SOURCES}
        src/ui/IconUtils.cpp
        resources/visnip_assets.qrc
    )
    target_include_directories(visnip_question_tests PRIVATE src)
    target_link_libraries(visnip_question_tests PRIVATE
        visnip_core Qt6::Concurrent Qt6::Widgets Qt6::Svg Qt6::Network Qt6::Test)
    if(WIN32)
        target_link_libraries(visnip_question_tests PRIVATE dwmapi)
    endif()
    visnip_enable_warnings(tests/tst_question.cpp)
    add_test(NAME visnip_question_tests COMMAND visnip_question_tests)
    set_tests_properties(visnip_question_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_FONTDIR=C:/Windows/Fonts"
    )
    # Lite offline translation: core rules plus the llama-server client, run
    # against a fake server so no model or GPU is needed.
    add_executable(visnip_fake_llama_server tests/fake_llama_server.cpp)
    target_link_libraries(visnip_fake_llama_server PRIVATE Qt6::Core Qt6::Network)
    add_executable(visnip_local_translation_tests
        tests/tst_local_translation.cpp
        src/services/LocalTextTranslationService.cpp
        src/services/TextTranslationService.cpp
    )
    target_include_directories(visnip_local_translation_tests PRIVATE src)
    target_link_libraries(visnip_local_translation_tests PRIVATE
        visnip_core Qt6::Concurrent Qt6::Network Qt6::Test)
    target_compile_definitions(visnip_local_translation_tests PRIVATE
        VISNIP_FAKE_LLAMA_SERVER="$<TARGET_FILE:visnip_fake_llama_server>")
    add_dependencies(visnip_local_translation_tests visnip_fake_llama_server)
    visnip_enable_warnings(tests/tst_local_translation.cpp tests/fake_llama_server.cpp)
    add_test(NAME visnip_local_translation_tests COMMAND visnip_local_translation_tests)
    set_tests_properties(visnip_local_translation_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
    add_executable(visnip_translation_review_tests tests/tst_translation_review.cpp)
    target_link_libraries(visnip_translation_review_tests PRIVATE visnip_ui Qt6::Test)
    add_test(NAME visnip_translation_review_tests COMMAND visnip_translation_review_tests)
    set_tests_properties(visnip_translation_review_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_FONTDIR=C:/Windows/Fonts")
    add_executable(visnip_translation_mode_tests
        tests/tst_translation_modes.cpp
        resources/visnip_assets.qrc
    )
    target_link_libraries(visnip_translation_mode_tests PRIVATE visnip_ui Qt6::Test)
    visnip_enable_warnings(tests/tst_translation_modes.cpp)
    add_test(NAME visnip_translation_mode_tests COMMAND visnip_translation_mode_tests)
    add_executable(visnip_offline_resource_tests tests/tst_offline_resources.cpp resources/visnip_assets.qrc)
    target_link_libraries(visnip_offline_resource_tests PRIVATE visnip_ui Qt6::Test)
    target_compile_definitions(visnip_offline_resource_tests PRIVATE VISNIP_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}/tests")
    visnip_enable_warnings(tests/tst_offline_resources.cpp)
    add_test(NAME visnip_offline_resource_tests COMMAND visnip_offline_resource_tests)
    set_tests_properties(visnip_offline_resource_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_FONTDIR=C:/Windows/Fonts")
    set_tests_properties(visnip_translation_mode_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_FONTDIR=C:/Windows/Fonts"
    )
    # QT_QPA_FONTDIR: the offscreen platform ships no fonts; the OCR test
    # paints text and needs real system fonts to do so.
    set_tests_properties(visnip_ui_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_FONTDIR=C:/Windows/Fonts"
    )
endif()

if(VISNIP_BUILD_TOOLS)
    add_executable(visnip_ocr_batch
        tools/ui_region_model/ocr_batch_main.cpp
    )
    target_link_libraries(visnip_ocr_batch PRIVATE visnip_ui)
    visnip_enable_warnings(tools/ui_region_model/ocr_batch_main.cpp)
endif()

install(TARGETS visnip RUNTIME DESTINATION bin)
