# blit from its sources, with Homebrew: this repository is its own tap.
#   brew tap ESiegman/blit https://github.com/ESiegman/blit
#   brew install --HEAD ESiegman/blit/blit
class Blit < Formula
  desc "Fast GPU text editor"
  homepage "https://github.com/ESiegman/blit"
  head "https://github.com/ESiegman/blit.git", branch: "main"

  depends_on "cmake" => :build
  depends_on "ninja" => :build
  depends_on "freetype"
  depends_on "gcc"
  depends_on "harfbuzz"
  depends_on "lua"
  depends_on :macos
  depends_on "sdl3"
  depends_on "tree-sitter"

  # Homebrew's cleaning of the keg breaks the bundle's signature (its seal
  # over Contents): blit.app is left as it was built and signed.
  skip_clean "blit.app"

  fails_with :clang do
    cause "blit is C++26, built with GCC (16 or later)"
  end

  def install
    system "cmake", "-S", ".", "-B", "build", "-G", "Ninja",
                    "-DCMAKE_CXX_COMPILER=#{ENV.cxx}",
                    "-DBLIT_NATIVE=OFF",
                    *std_cmake_args
    system "cmake", "--build", "build"
    prefix.install "build/blit.app"
    bin.install_symlink prefix/"blit.app/Contents/MacOS/blit"
  end

  def caveats
    <<~EOS
      blit.app is in #{opt_prefix}. To have it in Launchpad, Spotlight and the Dock:
        ln -sf #{opt_prefix}/blit.app /Applications/blit.app
    EOS
  end

  test do
    assert_match "usage: blit", shell_output("#{bin}/blit --help")
  end
end
