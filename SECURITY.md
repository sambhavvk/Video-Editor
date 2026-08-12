# Security Policy

This project is pre-beta. Please report suspected vulnerabilities privately to the maintainers
rather than opening a public issue with exploit details.

Media files, project files, subtitle files, LUTs, downloaded models, and worker messages are treated
as untrusted inputs. Parsers require bounds checks and fuzz coverage. Background workers receive
only the paths and immutable project revisions necessary for their job.

