# Maintainer: Ragul <rpohee@gmail.com>
#
# Tasks — a minimal 3-column kanban task manager for Omarchy (GTK4 GUI + CLI).
#
# This PKGBUILD builds from the files next to it, so it works straight from a
# clone:
#     makepkg -si
#
# For an AUR upload, switch to the released tarball (see the note at the
# bottom) and regenerate sha256sums with `updpkgsums`.

pkgname=tasks-gui
pkgver=1.0.0
pkgrel=1
pkgdesc="Minimal 3-column kanban task manager for Omarchy (GTK4 GUI + CLI)"
arch=('x86_64' 'aarch64')
url="https://github.com/dexter-morgan-oss/omarchy-tasks"
license=('MIT')
depends=('gtk4' 'jansson' 'python')
makedepends=('gcc' 'pkgconf')
install="$pkgname.install"
source=(
  "tasks-gui.c"
  "tasks.py"
  "taskstore.py"
  "tasks-setup-user"
  "tasks.svg"
  "LICENSE"
  "README.md"
  "GUIDE.md"
)
sha256sums=('SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP')

build() {
  gcc $CFLAGS -Wall -Wextra $(pkg-config --cflags gtk4 jansson) \
      tasks-gui.c -o tasks-gui $(pkg-config --libs gtk4 jansson) -lm
}

package() {
  install -Dm755 tasks-gui        "$pkgdir/usr/bin/tasks-gui"
  install -Dm755 tasks.py         "$pkgdir/usr/bin/tasks"
  install -Dm755 tasks-setup-user "$pkgdir/usr/bin/tasks-setup-user"
  install -Dm644 taskstore.py     "$pkgdir/usr/lib/omarchy-tasks/taskstore.py"
  install -Dm644 tasks.svg \
    "$pkgdir/usr/share/icons/hicolor/scalable/apps/org.omarchy.tasks.svg"
  install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
  install -Dm644 README.md GUIDE.md \
    -t "$pkgdir/usr/share/doc/$pkgname/"
}

# --- AUR release variant -----------------------------------------------------
# Replace the source/sha256sums arrays above with:
#   source=("$pkgname-$pkgver.tar.gz::https://github.com/dexter-morgan-oss/omarchy-tasks/archive/v$pkgver.tar.gz")
#   sha256sums=('<hash>')          # `updpkgsums` fills this in
# and change `cd` if needed; the build()/package() bodies stay the same.
