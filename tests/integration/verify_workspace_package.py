"""Independent byte comparison of an author snapshot and its runnable Python package."""
import argparse
import hashlib
import json
import os
from pathlib import Path


def verify(author, package, count=None, size=None, resource_size=None):
    def readable(path):
        absolute = str(path.resolve())
        if os.name == 'nt' and not absolute.startswith('\\\\?\\'):
            absolute = '\\\\?\\UNC\\' + absolute[2:] if absolute.startswith('\\\\') else '\\\\?\\' + absolute
        return Path(absolute)
    author, package = readable(author), readable(package)
    # Read the frozen directories directly, without importing any product or plugin code.
    sources = {'node.json': (author / 'node.json').read_bytes()}
    for folder in ('src', 'include'):
        sources.update({p.relative_to(author).as_posix(): p.read_bytes() for p in (author / folder).rglob('*') if p.is_file()})
    resources = json.loads(sources['node.json']).get('resources', [])
    sources.update({name: (author / name).read_bytes() for name in resources})
    resource_bytes = sum(len(sources[name]) for name in resources)
    if resource_size is not None:
        assert resource_bytes == resource_size
    if count is not None:
        assert len(sources) == count
    if size is not None:
        assert sum(map(len, sources.values())) == size
    manifest = json.loads((package / 'manifest.json').read_bytes())
    assert manifest['workspace_node_json'].encode() == sources['node.json']
    assert manifest['authoring_profile'] == {'kind': 'workspace-function-v1', 'syntax_only': False}
    expected = {'manifest.json': (package / 'manifest.json').read_bytes()}
    for name, data in sources.items():
        if name != 'node.json':
            installed = 'workspace_src/' + name.removeprefix('src/') if name.startswith('src/') else name
            assert (package / installed).read_bytes() == data
            expected[installed] = data
            if 'workspace_files' in manifest:
                assert manifest['workspace_files'][installed] == dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest(),
                    role='resource' if name in resources else 'source')
    snapshot = json.loads((package / 'snapshot.json').read_bytes())
    hashes = {name: hashlib.sha256(data).hexdigest() for name, data in expected.items()}
    assert snapshot['files'] == hashes
    signature = ''.join(name + '\0' + hashes[name] + '\n' for name in sorted(hashes)).encode()
    assert hashlib.sha256(signature).hexdigest() == snapshot['content_sha256'] == package.name
    actual = {p.relative_to(package).as_posix() for p in package.rglob('*') if p.is_file()}
    assert actual == set(expected) | {'snapshot.json'}
    assert len(expected) - len(resources) <= 128 and len(resources) <= 64
    assert sum(len(v) for k,v in expected.items() if k not in resources) <= 8 * 1024 * 1024
    assert sum(len(expected[k]) for k in resources) <= 64 * 1024 * 1024
    tree = 'AudioAlgoNodeWorkspace/1\n' + ''.join(name + '\n' + str(len(sources[name])) + '\n' +
        hashlib.sha256(sources[name]).hexdigest() + '\n' for name in sorted(sources))
    assert hashlib.sha256(tree.encode()).hexdigest() == manifest['workspace_content_sha256']
    return dict(author=str(author), package=str(package), author_files=len(sources),
                author_bytes=sum(map(len, sources.values())), package_files=len(expected),
                package_bytes=sum(map(len, expected.values())), package_sha256=package.name,
                resource_files=len(resources), resource_bytes=resource_bytes)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('author', type=Path)
    parser.add_argument('package', type=Path)
    parser.add_argument('--count', type=int)
    parser.add_argument('--bytes', type=int, dest='size')
    parser.add_argument('--resource-bytes', type=int)
    args = parser.parse_args()
    print(json.dumps(verify(args.author, args.package, args.count, args.size, args.resource_bytes), indent=2))
