# Maintainer: P Ragul <dexter-morgan-oss@users.noreply.github.com>
#
# Tasks — a minimal 3-column kanban task manager for Omarchy (GTK4 GUI + CLI).
#
# Builds from the released tag tarball, so `makepkg -si` works from anywhere
# (including a bare AUR checkout). No reclone needed.

pkgname=tasks-gui
pkgver=1.2.0
pkgrel=1
pkgdesc="Minimal 3-column kanban task manager for Omarchy (GTK4 GUI + CLI)"
arch=('x86_64' 'aarch64')
url="https://github.com/dexter-morgan-oss/omarchy-tasks"
license=('MIT')
depends=('gtk4' 'jansson' 'python')
makedepends=('gcc' 'pkgconf')
install="$pkgname.install"
source=("omarchy-tasks-$pkgver.tar.gz::https://github.com/dexter-morgan-oss/omarchy-tasks/archive/v$pkgver.tar.gz")
sha256sums=('b732fa038e986cc985d621c33115f85a52e4135ab5a621af0be01672329dce95')

build() {
  cd "omarchy-tasks-$pkgver"
  gcc $CFLAGS -Wall -Wextra $(pkg-config --cflags gtk4 jansson) \
      tasks-gui.c -o tasks-gui $(pkg-config --libs gtk4 jansson) -lm
}

package() {
  cd "omarchy-tasks-$pkgver"
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
