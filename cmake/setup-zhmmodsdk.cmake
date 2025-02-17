# If ZHMMODSDK_DIR is set, use that. Otherwise, download the SDK from GitHub.
if (DEFINED ZHMMODSDK_DIR)
    include("${ZHMMODSDK_DIR}/cmake/sdk-local.cmake")
else()
    include(FetchContent)
    cmake_policy(SET CMP0135 NEW)

    if (DEFINED ZHMMODSDK_ARTIFACT_ID AND DEFINED GITHUB_TOKEN)
        FetchContent_Declare(
            ZHMModSDK
            URL https://api.github.com/repos/OrfeasZ/ZHMModSDK/actions/artifacts/${ZHMMODSDK_ARTIFACT_ID}/zip
            HTTP_HEADER "Authorization: token $ENV{GITHUB_TOKEN}"
        )
    else()
        FetchContent_Declare(
            ZHMModSDK
            URL https://github.com/OrfeasZ/ZHMModSDK/releases/download/${ZHMMODSDK_VER}/DevPkg-ZHMModSDK.zip
        )
    endif()

    FetchContent_MakeAvailable(ZHMModSDK)
    set(ZHMMODSDK_DIR "${zhmmodsdk_SOURCE_DIR}")
endif()

include("${ZHMMODSDK_DIR}/cmake/zhm-mod.cmake")