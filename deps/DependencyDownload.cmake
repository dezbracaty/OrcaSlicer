# Verified HTTP downloads, compatible with CMake 3.16 and standalone consumers.
include_guard(GLOBAL)

function(gplatform_download URL DESTINATION)
    cmake_parse_arguments(PARSE_ARGV 2 DL "SHOW_PROGRESS"
        "EXPECTED_HASH;STATUS;LOG" "HTTPHEADER")
    if(DL_UNPARSED_ARGUMENTS OR DL_KEYWORDS_MISSING_VALUES OR
       NOT DL_EXPECTED_HASH MATCHES "^SHA256=([0-9A-Fa-f]+)$")
        message(FATAL_ERROR "gplatform_download requires an original SHA256 and supported download options")
    endif()
    string(TOLOWER "${CMAKE_MATCH_1}" _expected)
    string(LENGTH "${_expected}" _hash_length)
    if(NOT _hash_length EQUAL 64)
        message(FATAL_ERROR "gplatform_download requires a 64-digit SHA256")
    endif()

    set(_summary "")
    set(_verified FALSE)
    if(EXISTS "${DESTINATION}")
        file(SHA256 "${DESTINATION}" _actual)
        if(_actual STREQUAL _expected)
            set(_verified TRUE)
        endif()
    endif()
    if(_verified)
        if(DL_STATUS)
            set(${DL_STATUS} "0;SHA256 verified cached file" PARENT_SCOPE)
        endif()
        if(DL_LOG)
            set(${DL_LOG} "SHA256 verified cached file; no network request" PARENT_SCOPE)
        endif()
        return()
    endif()

    get_filename_component(_directory "${DESTINATION}" DIRECTORY)
    get_filename_component(_name "${DESTINATION}" NAME)
    if(NOT _directory)
        set(_directory "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
    file(MAKE_DIRECTORY "${_directory}")
    # Hidden names cannot be mistaken for extracted sources by boost-* globs.
    set(_partial "${_directory}/.${_name}.part")
    file(LOCK "${_directory}/.${_name}.download.lock"
        GUARD FUNCTION TIMEOUT 2720 RESULT_VARIABLE _lock_result)
    if(NOT _lock_result STREQUAL "0")
        if(DL_STATUS)
            set(${DL_STATUS} "1;Download cache lock unavailable" PARENT_SCOPE)
        endif()
        if(DL_LOG)
            set(${DL_LOG} "Cannot acquire download cache lock: ${_lock_result}" PARENT_SCOPE)
        endif()
        return()
    endif()

    # Another architecture may have populated the shared archive while waiting.
    foreach(_candidate IN ITEMS "${DESTINATION}" "${_partial}")
        if(EXISTS "${_candidate}")
            file(SHA256 "${_candidate}" _actual)
            if(_actual STREQUAL _expected)
                if(_candidate STREQUAL _partial)
                    file(RENAME "${_partial}" "${DESTINATION}")
                endif()
                if(DL_STATUS)
                    set(${DL_STATUS} "0;SHA256 verified cached file" PARENT_SCOPE)
                endif()
                if(DL_LOG)
                    set(${DL_LOG} "SHA256 verified cached file; no network request" PARENT_SCOPE)
                endif()
                return()
            elseif(_candidate STREQUAL DESTINATION)
                # Only this lock owner may remove a known-invalid formal file.
                # A caller must not unlink it after releasing our lock: another
                # architecture may already have committed a valid replacement.
                file(REMOVE "${DESTINATION}")
            endif()
        endif()
    endforeach()

    # Resolve a host tool only when an actual download is required. No curlrc,
    # nested curl retry, shell interpolation, or TLS-verification override.
    find_program(GPLATFORM_DOWNLOAD_CURL NAMES curl curl.exe NO_CMAKE_FIND_ROOT_PATH)
    if(NOT GPLATFORM_DOWNLOAD_CURL)
        if(DL_STATUS)
            set(${DL_STATUS} "1;curl is required to download this dependency" PARENT_SCOPE)
        endif()
        if(DL_LOG)
            set(${DL_LOG} "Install a host curl executable or set GPLATFORM_DOWNLOAD_CURL" PARENT_SCOPE)
        endif()
        return()
    endif()
    set(_headers)
    foreach(_header IN LISTS DL_HTTPHEADER)
        list(APPEND _headers --header "${_header}")
    endforeach()
    set(_progress --silent --show-error)
    if(DL_SHOW_PROGRESS)
        set(_progress --show-error)
    endif()

    foreach(_attempt RANGE 1 3)
        message(STATUS "Downloading ${_name}: attempt ${_attempt}/3 (HTTP/1.1)")
        execute_process(
            COMMAND "${GPLATFORM_DOWNLOAD_CURL}" -q --http1.1 --location --fail
                ${_progress} --connect-timeout 30 --max-time 900
                --speed-limit 1024 --speed-time 60 --continue-at -
                --output "${_partial}" --write-out "%{http_code}"
                ${_headers} "${URL}"
            RESULT_VARIABLE _curl_result OUTPUT_VARIABLE _http_status
            TIMEOUT 905)
        string(STRIP "${_http_status}" _http_status)
        set(_actual "")
        if(EXISTS "${_partial}")
            file(SHA256 "${_partial}" _actual)
        endif()
        # Even a connection-close error can arrive after the complete payload.
        # Only the pinned hash, never a transport exit code alone, admits it.
        if(_actual STREQUAL _expected)
            file(RENAME "${_partial}" "${DESTINATION}")
            set(_verified TRUE)
            string(APPEND _summary "Attempt ${_attempt}: original SHA256 verified.\n")
            break()
        endif()
        string(APPEND _summary
            "Attempt ${_attempt}: curl=${_curl_result}, HTTP=${_http_status}; SHA256 not verified.\n")
        # A completed wrong payload must not be resumed. Range rejection also
        # consumes an attempt; a fresh transfer uses only the remaining budget.
        if(_curl_result STREQUAL "0" OR _curl_result STREQUAL "33" OR
           _http_status STREQUAL "416")
            file(REMOVE "${_partial}")
        endif()
        if(_attempt LESS 3)
            execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep "${_attempt}")
        endif()
    endforeach()
    if(DL_STATUS)
        if(_verified)
            set(${DL_STATUS} "0;Original SHA256 verified" PARENT_SCOPE)
        else()
            set(${DL_STATUS} "1;Download failed after 3 attempts; original SHA256 not verified" PARENT_SCOPE)
        endif()
    endif()
    if(DL_LOG)
        set(${DL_LOG} "${_summary}" PARENT_SCOPE)
    endif()
endfunction()
