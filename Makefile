# Copyright 2026 PolitePixels Limited
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

.POSIX:

.PHONY: all test assets clean paper paper-pages

BUILD ?= build

all:
	@echo "targets: test, assets, clean"
	@echo "for the full receipt set:      ./reproduce.sh"
	@echo "for the must-fail controls:    ./controls.sh"
	@echo "to see the check table:        ./reproduce.sh --list"

test:
	@$(MAKE) -C src/lanelang test

assets:
	@$(MAKE) -C assets assets

clean:
	rm -rf $(BUILD)

paper:
	$(MAKE) -C paper all

paper-pages:
	@$(MAKE) -C paper pages
