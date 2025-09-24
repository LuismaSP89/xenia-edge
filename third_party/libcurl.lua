group("third_party")

if os.istarget("linux") then
  -- On Linux, we don't create a project at all - projects will link system curl directly
  -- This is handled by adding "curl" to the links in projects that need it
else
  -- On Windows, build from source with native Schannel SSL
  project("libcurl")
    uuid("1ba7e608-5752-457c-8df0-c006c6e8b7fe")
    kind("StaticLib")
    language("C")

    links({
      "crypt32",
      "secur32",
      "ws2_32",
      "wldap32",
      "advapi32",
    })
    defines({
      "BUILDING_LIBCURL",
      "USE_SCHANNEL",  -- Use Windows native SSL
      "USE_WINDOWS_SSPI",  -- Use Windows SSPI for authentication
      "CURL_DISABLE_LDAP",  -- Disable LDAP to simplify
      "CURL_STATICLIB",
    })
    includedirs({
      "libcurl/lib",
      "libcurl/include",
    })
    files({
      "libcurl/lib/**.h",
      "libcurl/lib/**.c",
    })
end