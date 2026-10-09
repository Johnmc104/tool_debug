#!/usr/bin/env python3
"""提取公开 CentOS 7 镜像中的运行库，供无挂载 chroot 验证使用。"""
import argparse
import hashlib
import json
from pathlib import Path
import tarfile
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('build/centos7_backend'))
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    url = 'https://quay.io/v2/centos/centos/manifests/7'
    request = urllib.request.Request(url, headers={'Accept': 'application/vnd.docker.distribution.manifest.v2+json'})
    with urllib.request.urlopen(request, timeout=30) as response:
        raw = response.read()
    (output / 'remote_manifest.json').write_bytes(raw)
    manifest = json.loads(raw)
    root = output / 'rootfs'
    root.mkdir(exist_ok=True)
    for index, layer in enumerate(manifest['layers']):
        digest = layer['digest'].split(':', 1)[1]
        archive = output / 'centos-layer-{}.tar.gz'.format(index)
        if not archive.exists() or hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
            with urllib.request.urlopen('https://quay.io/v2/centos/centos/blobs/sha256:' + digest, timeout=30) as response:
                with archive.open('wb') as stream:
                    while True:
                        chunk = response.read(1024 * 1024)
                        if not chunk:
                            break
                        stream.write(chunk)
        assert hashlib.sha256(archive.read_bytes()).hexdigest() == digest
        with tarfile.open(str(archive)) as stream:
            for member in stream.getmembers():
                name = member.name.lstrip('./')
                if member.name.startswith('/') or '..' in Path(member.name).parts:
                    continue
                if not (name in ['lib64', 'usr/lib64', 'etc/centos-release'] or name.startswith('usr/lib64/')):
                    continue
                if member.issym() and (member.linkname.startswith('/') or '..' in Path(member.linkname).parts):
                    continue
                if member.islnk() and (member.linkname.startswith('/') or '..' in Path(member.linkname).parts):
                    continue
                if not (member.isfile() or member.isdir() or member.issym() or member.islnk()):
                    continue
                stream.extract(member, str(root))
        print('Verified layer sha256:' + digest)
    print((root / 'etc/centos-release').read_text().strip())
    print(root)


if __name__ == '__main__':
    main()
