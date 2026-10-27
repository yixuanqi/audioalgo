"""Runner-owned adapter for a frozen multi-file function workspace."""
from __future__ import annotations
from .sdk import AudioPort, DiagnosticError
from .source_loader import load_module


def node_class(info, snapshot, digest, files, profile):
    if (not isinstance(profile, dict) or set(profile) != {'kind', 'syntax_only'} or
            profile.get('kind') != 'workspace-function-v1' or type(profile.get('syntax_only')) is not bool):
        raise DiagnosticError('python_authoring_profile_invalid', 'Unknown or invalid workspace function profile')
    module_name, function_name = info.descriptor['entrypoint'].split(':')
    if not module_name.startswith('workspace_src.'):
        raise DiagnosticError('python_authoring_profile_invalid', 'Workspace entrypoint must be under workspace_src')

    class Node:
        @classmethod
        def describe(cls):
            return info.descriptor

        def prepare(self, context):
            self.state = {}
            self.module = None
            # Compile the entire frozen tree before importing any user module.
            for name in sorted(files):
                if not name.endswith('.py'):
                    continue
                path = snapshot / name
                try:
                    compile(path.read_bytes(), str(path), 'exec', dont_inherit=True)
                except SyntaxError as error:
                    relative = 'src/' + name[len('workspace_src/'):] if name.startswith('workspace_src/') else name
                    raise DiagnosticError('python_syntax_error',
                        f'{relative}:{error.lineno or 0}:{error.offset or 0}: {error.msg}') from None
            if profile['syntax_only']:
                return
            self.module = load_module(info.plugin_id, digest, snapshot, files, module_name)
            self.function = getattr(self.module, function_name, None)
            if not callable(self.function):
                raise DiagnosticError('authoring_entrypoint_missing',
                    f'{module_name}:{function_name} must define a callable function')
            initialize = getattr(self.module, 'initialize', None)
            if initialize is not None:
                initialize(context.parameters, self.state)

        def process(self, inputs, context):
            reference = next(iter(inputs.values()))
            if profile['syntax_only']:
                return {port['id']: AudioPort(reference.samples, reference.sample_rate_hz,
                    reference.channel_ids, reference.valid_interval) for port in self.describe()['outputs']}
            result = self.function({key: value.samples for key, value in inputs.items()}, context.parameters, self.state)
            if not isinstance(result, dict):
                raise DiagnosticError('python_output_ports_invalid', 'process must return a dict keyed by declared output port')
            return {key: AudioPort(value, reference.sample_rate_hz, reference.channel_ids, reference.valid_interval)
                for key, value in result.items()}

        def reset(self, reason):
            reset = getattr(self.module, 'reset', None)
            if reset is not None:
                reset(self.state, reason)
            self.state = {}

    return Node
