(*
  Copyright 2026 PolitePixels Limited

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*)

Require Import List.
Require Import Arith.
Require Import Lia.
Import ListNotations.

(**
  [lane] orders the three guarantees from [Exact] through [Replay] to [Approx].
*)
Inductive lane : Type :=
  (**
    [Exact] requires agreement across all schedules, including the serial oracle.
  *)
  | Exact  : lane

  (**
    [Replay] requires agreement between any two gated schedules.
  *)
  | Replay : lane

  (**
    [Approx] imposes no equality requirement across schedules.
  *)
  | Approx : lane.

(**
  [join] gives the least upper bound of two lanes. This is the guarantee a consumer
  can retain when both lanes contribute to its result.

  Takes [a] and [b] ([lane]) which are the lanes to combine.
*)
Definition join (a b : lane) : lane :=
  match a, b with
  | Approx, _      => Approx
  | _, Approx      => Approx
  | Replay, _      => Replay
  | _, Replay      => Replay
  | Exact, Exact   => Exact
  end.

(**
  [lane_le] orders lanes by the guarantees they provide. An output at [a] can meet a
  demand for [b] when their join is [b].

  Takes [a] and [b] ([lane]) which are the two lanes being compared.
*)
Definition lane_le (a b : lane) : Prop := join a b = b.

(**
  [a <=l b] is the infix form of [lane_le a b]. The stronger guarantee is on the
  left.
*)
Infix "<=l" := lane_le (at level 70).

(**
  [join_idem] shows that a repeated dependency on [a] leaves its lane unchanged.

  Takes [a] ([lane]) which is the lane under consideration.
*)
Lemma join_idem : forall a, join a a = a.
Proof. destruct a; reflexivity. Qed.

(**
  [join_exact_l] shows that adding an [Exact] input leaves the other lane unchanged.

  Takes [a] ([lane]) which is the lane under consideration.
*)
Lemma join_exact_l : forall a, join Exact a = a.
Proof. destruct a; reflexivity. Qed.

(**
  [exact_bottom] places [Exact] below every lane in the order.

  Takes [a] ([lane]) which is the lane under consideration.
*)
Lemma exact_bottom : forall a, Exact <=l a.
Proof.
  intro a.
  unfold lane_le.
  apply join_exact_l.
Qed.

(**
  [join_approx_l] shows that an [Approx] operand makes the join [Approx] for every
  other lane.

  Takes [a] ([lane]) which is the lane under consideration.
*)
Lemma join_approx_l : forall a, join Approx a = Approx.
Proof. destruct a; reflexivity. Qed.

(**
  [le_refl] proves that a lane satisfies its own demand.

  Takes [a] ([lane]) which is the lane under consideration.
*)
Lemma le_refl : forall a, a <=l a.
Proof.
  intro a.
  unfold lane_le.
  apply join_idem.
Qed.

(**
  [le_trans] composes two lane comparisons through the common middle lane.

  Takes [a] and [b] ([lane]) which are the first and middle lanes.
  Takes [c] ([lane]) which is the final lane.
*)
Lemma le_trans : forall a b c, a <=l b -> b <=l c -> a <=l c.
Proof.
  destruct a, b, c; unfold lane_le; simpl; congruence.
Qed.

(**
  [join_ub_l] shows that a join is an upper bound for its left operand.

  Takes [a] and [b] ([lane]) which are the left and right operands respectively.
*)
Lemma join_ub_l : forall a b, a <=l join a b.
Proof. destruct a, b; reflexivity. Qed.

(**
  [join_ub_r] shows that a join is an upper bound for its right operand.

  Takes [a] and [b] ([lane]) which are the left and right operands respectively.
*)
Lemma join_ub_r : forall a b, b <=l join a b.
Proof. destruct a, b; reflexivity. Qed.

(**
  [join_lub] proves that any common upper bound of the two operands also bounds
  their join.

  Takes [a] and [b] ([lane]) which are the lanes being joined.
  Takes [c] ([lane]) which is their proposed upper bound.
*)
Lemma join_lub : forall a b c, a <=l c -> b <=l c -> join a b <=l c.
Proof.
  destruct a, b, c; unfold lane_le; simpl; congruence.
Qed.

(**
  [schedule] is the abstract type of execution choices compared at a fixed world.
*)
Parameter schedule : Type.

(**
  [Gated] identifies the schedules covered by the replay guarantee.
*)
Parameter Gated : schedule -> Prop.

(**
  [oracle] is the distinguished serial reference schedule.
*)
Parameter oracle : schedule.

(**
  [oracle_ungated] places the serial reference outside the replay gate. This supplies
  an exact schedule comparison that replay excludes.
*)
Axiom oracle_ungated : ~ Gated oracle.

(**
  [some_gated] supplies a schedule that replay can compare with itself. This
  witnesses that the replay comparison set is non-empty.
*)
Axiom some_gated : exists s, Gated s.

(**
  [world] represents the external state held fixed during a schedule comparison.
*)
Parameter world : Type.

(**
  [agrees] specifies which pairs of schedules a lane must compare. [Exact] compares
  every pair. [Replay] compares pairs of gated schedules. [Approx] imposes no
  comparisons.

  Takes [l] ([lane]) which is the guarantee under consideration.
  Takes [s] and [s'] ([schedule]) which are the schedules of the two runs.
*)
Definition agrees (l : lane) (s s' : schedule) : Prop :=
  match l with
  | Exact  => True
  | Replay => Gated s /\ Gated s'
  | Approx => False
  end.

(**
  [agrees_exact_strictly_contains_replay] shows that exact comparisons include every
  replay comparison and also include the ungated oracle paired with itself.
*)
Lemma agrees_exact_strictly_contains_replay :
  (forall s s', agrees Replay s s' -> agrees Exact s s') /\
  (exists s s', agrees Exact s s' /\ ~ agrees Replay s s').
Proof.
  split.
  - intros s s' _.
    exact I.
  - exists oracle, oracle.
    split.
    + exact I.
    + intros [Hg _].
      exact (oracle_ungated Hg).
Qed.

(**
  [agrees_replay_strictly_contains_approx] uses a gated schedule to witness a pair
  that replay compares and approximate execution leaves unconstrained.
*)
Lemma agrees_replay_strictly_contains_approx :
  (forall s s', agrees Approx s s' -> agrees Replay s s') /\
  (exists s s', agrees Replay s s' /\ ~ agrees Approx s s').
Proof.
  split.
  - intros s s' [].
  - destruct some_gated as [s Hs].
    exists s, s.
    split.
    + split; exact Hs.
    + intros [].
Qed.

(**
  [agrees_antitone] transfers schedule agreement at the weaker guarantee to
  agreement at the stronger one.

  Takes [a] and [b] ([lane]) which are the stronger and weaker guarantees
  respectively.
  Takes [s] and [s'] ([schedule]) which are the schedules of the two runs.
*)
Lemma agrees_antitone :
  forall a b s s', a <=l b -> agrees b s s' -> agrees a s s'.
Proof.
  destruct a, b; unfold lane_le; simpl; try tauto; discriminate.
Qed.

(**
  [node] records the operation's lane and its ordinary inputs. A recurrence may also
  have a carried edge. The optional declaration records the lane demanded at this
  node. [mkNode] builds the record from the operation lane, input indices, optional
  declaration, source flag and optional carried index, in that order.
*)
Record node : Type := mkNode {
  (**
    [op_lane] is the lane contributed by the operation before its inputs are
    considered.
  *)
  op_lane  : lane;

  (**
    [inputs] lists the indices of the node's ordinary dependencies in operand order.
  *)
  inputs   : list nat;

  (**
    [declared] holds the lane demanded at this node when the source contains a
    declaration.
  *)
  declared : option lane;

  (**
    [source] marks a node whose value may depend directly on the external world.
  *)
  source   : bool;

  (**
    [carried] identifies the dependency that supplies the carried value of a
    recurrence.
  *)
  carried  : option nat
}.

(**
  [prog] represents a graph as a list of nodes indexed from zero.
*)
Definition prog := list node.

(**
  [wf] requires each ordinary input to precede its consumer. A carried edge may point
  forward, which is why graph induction needs a separate premise for recurrence.

  Takes [P] ([prog]) which is the graph in evaluation order.
*)
Definition wf (P : prog) : Prop :=
  forall i n j, nth_error P i = Some n -> In j (inputs n) -> j < i.

(**
  [bits] is the abstract result type whose equality expresses bitwise agreement.
*)
Parameter bits : Type.

(**
  [eval] is the abstract evaluator. Its arguments identify the graph, node, schedule
  and world, in that order. Its result represents the node's output bits.
*)
Parameter eval : prog -> nat -> schedule -> world -> bits.

(**
  [Assumption1] states the local premise of Theorem 1. At a fixed world, a node
  preserves equality of its ordinary and carried inputs whenever the schedules agree
  at its operation lane.

  Takes [P] ([prog]) which is the graph on which the evaluator must satisfy the
  premise.
*)
Definition Assumption1 (P : prog) : Prop :=
  forall i n s s' w,
    nth_error P i = Some n ->
    agrees (op_lane n) s s' ->
    (forall j, In j (inputs n) -> eval P j s w = eval P j s' w) ->
    (forall c, carried n = Some c -> eval P c s w = eval P c s' w) ->
    eval P i s w = eval P i s' w.

(**
  [carried_lane] reads the lane of the carried input. A missing carried edge
  contributes [Exact], the identity of the join.

  Takes [L] ([nat -> lane]) which assigns a lane to each node index.
  Takes [n] ([node]) which is the consumer whose lane is computed.
*)
Definition carried_lane (L : nat -> lane) (n : node) : lane :=
  match carried n with
  | Some c => L c
  | None   => Exact
  end.

(**
  [node_lane] joins the operation lane with the lanes of its ordinary inputs and its
  carried input, when present.

  Takes [L] ([nat -> lane]) which is the assignment used to look up input lanes.
  Takes [n] ([node]) which is the operation whose result lane is computed.
*)
Definition node_lane (L : nat -> lane) (n : node) : lane :=
  join (carried_lane L n) (fold_left join (map L (inputs n)) (op_lane n)).

(**
  [sound_assignment] requires every node's assigned lane to equal the join computed
  by [node_lane].

  Takes [P] ([prog]) which is the classified graph.
  Takes [L] ([nat -> lane]) which is the assignment being checked.
*)
Definition sound_assignment (P : prog) (L : nat -> lane) : Prop :=
  forall i n, nth_error P i = Some n -> L i = node_lane L n.

(**
  [fold_join_ge_seed] shows that joining more lanes can only move the result upwards
  from its initial lane.

  Takes [l] ([list lane]) which holds the lanes to join.
  Takes [a] ([lane]) which is the initial lane.
*)
Lemma fold_join_ge_seed :
  forall l a, a <=l fold_left join l a.
Proof.
  induction l as [| x xs IH]; intro a; simpl.
  - apply le_refl.
  - eapply le_trans.
    + apply join_ub_l.
    + apply IH.
Qed.

(**
  [fold_join_ge_elem] proves that the folded join bounds every lane present in the
  input list.

  Takes [l] ([list lane]) which holds the lanes to join.
  Takes [a] ([lane]) which is the initial lane.
  Takes [x] ([lane]) which is the list member being bounded.
*)
Lemma fold_join_ge_elem :
  forall l a x, In x l -> x <=l fold_left join l a.
Proof.
  induction l as [| y ys IH]; intros a x Hin; simpl in *.
  - contradiction.
  - destruct Hin as [-> | Hin].
    + eapply le_trans.
      * apply join_ub_r.
      * apply fold_join_ge_seed.
    + apply IH.
      exact Hin.
Qed.

(**
  [node_lane_ge_op] proves that the result lane bounds the operation's own lane.

  Takes [L] ([nat -> lane]) which assigns a lane to each node index.
  Takes [n] ([node]) which is the consumer whose lane is computed.
*)
Lemma node_lane_ge_op : forall L n, op_lane n <=l node_lane L n.
Proof.
  intros L n.
  unfold node_lane.
  eapply le_trans.
  - apply fold_join_ge_seed.
  - apply join_ub_r.
Qed.

(**
  [node_lane_ge_input] proves that the result lane bounds the lane of each ordinary
  input.

  Takes [L] ([nat -> lane]) which assigns a lane to each node index.
  Takes [n] ([node]) which is the consumer whose lane is computed.
  Takes [j] ([nat]) which identifies the ordinary input.
*)
Lemma node_lane_ge_input :
  forall L n j, In j (inputs n) -> L j <=l node_lane L n.
Proof.
  intros L n j Hin.
  unfold node_lane.
  eapply le_trans.
  - apply fold_join_ge_elem, in_map, Hin.
  - apply join_ub_r.
Qed.

(**
  [node_lane_ge_carried] proves that the result lane also bounds its carried
  dependency.

  Takes [L] ([nat -> lane]) which assigns a lane to each node index.
  Takes [n] ([node]) which is the consumer whose lane is computed.
  Takes [c] ([nat]) which identifies the carried input.
*)
Lemma node_lane_ge_carried :
  forall L n c, carried n = Some c -> L c <=l node_lane L n.
Proof.
  intros L n c Hc.
  unfold node_lane, carried_lane.
  rewrite Hc.
  apply join_ub_l.
Qed.

(**
  [carried_agrees] supplies the recurrence premise for Theorem 1. Agreement at a
  consumer's lane requires equal bits at its carried input across the two runs.

  Takes [P] ([prog]) which is the graph containing the recurrence.
  Takes [L] ([nat -> lane]) which is the assignment of lanes to its nodes.
  Takes [s] and [s'] ([schedule]) which are the schedules being compared.
  Takes [w] ([world]) which is the external state shared by both runs.
*)
Definition carried_agrees (P : prog) (L : nat -> lane) (s s' : schedule) (w : world) : Prop :=
  forall i n c,
    nth_error P i = Some n ->
    carried n = Some c ->
    agrees (L i) s s' ->
    eval P c s w = eval P c s' w.

(**
  [theorem1] proves schedule agreement for a node of a well-formed graph with a sound
  lane assignment. Assuming [Assumption1] and [carried_agrees], schedules that agree
  at the node's lane produce equal bits in the same world.

  Takes [P] ([prog]) which is the classified graph.
  Takes [L] ([nat -> lane]) which is the assignment of lanes to its nodes.
  Takes [i] ([nat]) which is the node index whose output is compared.
  Takes [n] ([node]) which is the node at that index.
  Takes [s] and [s'] ([schedule]) which are the schedules of the two runs.
  Takes [w] ([world]) which is the external state shared by both runs.
*)
Theorem theorem1 :
  forall P L,
    wf P ->
    Assumption1 P ->
    sound_assignment P L ->
    forall i n s s' w,
      nth_error P i = Some n ->
      carried_agrees P L s s' w ->
      agrees (L i) s s' ->
      eval P i s w = eval P i s' w.
Proof.
  intros P L Hwf HA1 Hsound i.
  induction i as [i IH] using (well_founded_induction lt_wf).
  intros n s s' w Hnth Hcar Hag.
  rewrite (Hsound i n Hnth) in Hag.

  apply (HA1 i n s s' w Hnth).
  - eapply agrees_antitone.
    { apply node_lane_ge_op. }
    exact Hag.
  - intros j Hin.
    assert (Hlt : j < i) by exact (Hwf i n j Hnth Hin).
    destruct (nth_error P j) as [nj |] eqn:Hj.
    + apply (IH j Hlt nj s s' w Hj Hcar).
      eapply agrees_antitone.
      { apply (node_lane_ge_input L n j Hin). }
      exact Hag.
    + exfalso.
      apply nth_error_None in Hj.
      assert (Hlen : i < length P).
      { apply nth_error_Some.
        rewrite Hnth.
        discriminate. }
      lia.
  - intros c Hc.
    apply (Hcar i n c Hnth Hc).
    rewrite (Hsound i n Hnth).
    exact Hag.
Qed.

(**
  [grounded_assignment] states the local grounding rule. A node is marked grounded
  exactly when it is a non-source and its ordinary and carried inputs are grounded.

  Takes [P] ([prog]) which is the graph under analysis.
  Takes [G] ([nat -> bool]) which is the proposed grounding mark for each node.
*)
Definition grounded_assignment (P : prog) (G : nat -> bool) : Prop :=
  forall i n,
    nth_error P i = Some n ->
    G i = andb (negb (source n))
               (andb (forallb G (inputs n))
                     (match carried n with Some c => G c | None => true end)).

(**
  [BlessHolds] requires each blessed node to produce equal bits in the two worlds at
  a fixed schedule.

  Takes [P] ([prog]) which is the graph being evaluated.
  Takes [B] ([nat -> bool]) which identifies the blessed nodes.
  Takes [s] ([schedule]) which is the schedule shared by both runs.
  Takes [w] and [w'] ([world]) which are the external states being compared.
*)
Definition BlessHolds (P : prog) (B : nat -> bool) (s : schedule) (w w' : world) : Prop :=
  forall i, B i = true -> eval P i s w = eval P i s w'.

(**
  [waived_assignment] allows a blessing to discharge the source condition at a node.
  Its ordinary and carried inputs must still pass the same check.

  Takes [P] ([prog]) which is the graph under analysis.
  Takes [B] ([nat -> bool]) which identifies the blessed nodes.
  Takes [C] ([nat -> bool]) which is the proposed assignment after waivers.
*)
Definition waived_assignment (P : prog) (B C : nat -> bool) : Prop :=
  forall i n,
    nth_error P i = Some n ->
    C i = andb (orb (negb (source n)) (B i))
               (andb (forallb C (inputs n))
                     (match carried n with Some c => C c | None => true end)).

(**
  [grounded_is_waived_unblessed] recovers the ordinary grounding equation when the
  set of blessings is empty.

  Takes [P] ([prog]) which is the graph being classified.
  Takes [G] ([nat -> bool]) which is the proposed grounding assignment.
*)
Lemma grounded_is_waived_unblessed :
  forall P G, grounded_assignment P G <-> waived_assignment P (fun _ => false) G.
Proof.
  intros P G.
  unfold grounded_assignment, waived_assignment.
  split; intros H i n Hnth; specialize (H i n Hnth).
  all: rewrite H; simpl; destruct (source n); reflexivity.
Qed.

(**
  [Assumption2] states the local premise for grounding. At a fixed schedule, equal
  ordinary and carried inputs give a non-source node equal results across worlds.

  Takes [P] ([prog]) which is the graph on which the evaluator must satisfy the
  premise.
*)
Definition Assumption2 (P : prog) : Prop :=
  forall i n s w w',
    nth_error P i = Some n ->
    source n = false ->
    (forall j, In j (inputs n) -> eval P j s w = eval P j s w') ->
    (forall c, carried n = Some c -> eval P c s w = eval P c s w') ->
    eval P i s w = eval P i s w'.

(**
  [carried_world_agrees] requires equality across worlds at each carried input marked
  by the assignment.

  Takes [P] ([prog]) which is the graph containing the carried edges.
  Takes [G] ([nat -> bool]) which identifies the nodes whose grounding or waiver is
  assumed.
  Takes [s] ([schedule]) which is the common schedule.
  Takes [w] and [w'] ([world]) which are the external states being compared.
*)
Definition carried_world_agrees (P : prog) (G : nat -> bool) (s : schedule) (w w' : world) : Prop :=
  forall i n c,
    nth_error P i = Some n ->
    carried n = Some c ->
    G c = true ->
    eval P c s w = eval P c s w'.

(**
  [waived_sound] proves equality across worlds at a node marked by the waiver
  assignment on a well-formed graph. It assumes [Assumption2], valid blessings
  for the two worlds and agreement at the marked carried inputs.

  Takes [P] ([prog]) which is the graph being checked.
  Takes [B] ([nat -> bool]) which identifies the blessed nodes.
  Takes [C] ([nat -> bool]) which identifies the nodes accepted by the waiver
  assignment.
  Takes [i] ([nat]) which is the index of the node under consideration.
  Takes [n] ([node]) which is the node at that index.
  Takes [s] ([schedule]) which is the schedule shared by both runs.
  Takes [w] and [w'] ([world]) which are the external states being compared.
*)
Theorem waived_sound :
  forall P B C,
    wf P ->
    Assumption2 P ->
    waived_assignment P B C ->
    forall i n s w w',
      nth_error P i = Some n ->
      BlessHolds P B s w w' ->
      carried_world_agrees P C s w w' ->
      C i = true ->
      eval P i s w = eval P i s w'.
Proof.
  intros P B C Hwf HA2 Hcov i.
  induction i as [i IH] using (well_founded_induction lt_wf).
  intros n s w w' Hnth Hbless Hcar Hc.
  rewrite (Hcov i n Hnth) in Hc.
  apply andb_prop in Hc.
  destruct Hc as [Hhead Hrest].
  apply andb_prop in Hrest.
  destruct Hrest as [Hins Hcarried].

  destruct (B i) eqn:Hb.
  - exact (Hbless i Hb).
  - assert (Hsrc : source n = false).
    { destruct (source n).
      - simpl in Hhead.
        discriminate.
      - reflexivity. }

    apply (HA2 i n s w w' Hnth).
    + exact Hsrc.
    + intros j Hin.
      assert (Hlt : j < i) by exact (Hwf i n j Hnth Hin).
      destruct (nth_error P j) as [nj |] eqn:Hj.
      * apply (IH j Hlt nj s w w' Hj Hbless Hcar).
        rewrite forallb_forall in Hins.
        exact (Hins j Hin).
      * exfalso.
        apply nth_error_None in Hj.
        assert (Hlen : i < length P).
        { apply nth_error_Some.
          rewrite Hnth.
          discriminate. }
        lia.
    + intros c Hc'.
      apply (Hcar i n c Hnth Hc').
      rewrite Hc' in Hcarried.
      exact Hcarried.
Qed.

(**
  [grounding_sound] proves equality across worlds at a grounded node of a well-formed
  graph. Both runs use the same schedule, and the proof assumes [Assumption2] and
  agreement at grounded carried inputs.

  Takes [P] ([prog]) which is the graph being checked.
  Takes [G] ([nat -> bool]) which identifies the nodes marked grounded.
  Takes [i] ([nat]) which is the index of the node under consideration.
  Takes [n] ([node]) which is the node at that index.
  Takes [s] ([schedule]) which is the schedule shared by both runs.
  Takes [w] and [w'] ([world]) which are the external states being compared.
*)
Theorem grounding_sound :
  forall P G,
    wf P ->
    Assumption2 P ->
    grounded_assignment P G ->
    forall i n s w w',
      nth_error P i = Some n ->
      carried_world_agrees P G s w w' ->
      G i = true ->
      eval P i s w = eval P i s w'.
Proof.
  intros P G Hwf HA2 Hg i n s w w' Hnth Hcar Hgi.
  apply (waived_sound P (fun _ => false) G Hwf HA2
           (proj1 (grounded_is_waived_unblessed P G) Hg) i n s w w' Hnth).
  - intros j Hj.
    discriminate.
  - exact Hcar.
  - exact Hgi.
Qed.

(**
  [lone_source] is the single external node used to distinguish grounding from a
  waiver.
*)
Definition lone_source : node := mkNode Exact [] None true None.

(**
  [blessed_source_is_waived] shows that a waiver assignment accepts the lone source
  when it is blessed.

  Takes [C] ([nat -> bool]) which is the assignment after waivers.
*)
Lemma blessed_source_is_waived :
  forall C, waived_assignment [lone_source] (fun _ => true) C -> C 0 = true.
Proof.
  intros C H.
  specialize (H 0 lone_source eq_refl).
  exact H.
Qed.

(**
  [unblessed_source_is_not_grounded] shows that the grounding rule leaves the lone
  external source ungrounded.

  Takes [G] ([nat -> bool]) which is its grounding assignment.
*)
Lemma unblessed_source_is_not_grounded :
  forall G, grounded_assignment [lone_source] G -> G 0 = false.
Proof.
  intros G H.
  specialize (H 0 lone_source eq_refl).
  exact H.
Qed.

(**
  [grounding_accepted] requires each node with a grounding demand to pass the
  supplied assignment.

  Takes [Demands] ([nat -> bool]) which identifies nodes with a grounding demand.
  Takes [C] ([nat -> bool]) which identifies nodes that pass the check.
*)
Definition grounding_accepted (Demands C : nat -> bool) : Prop :=
  forall i, Demands i = true -> C i = true.

(**
  [waived_declaration_sound] applies [waived_sound] to a node whose grounding demand
  the waiver assignment accepts. The conclusion depends on the blessings holding in
  the two worlds.

  Takes [P] ([prog]) which is the graph being checked.
  Takes [B] ([nat -> bool]) which identifies the blessed nodes.
  Takes [C] ([nat -> bool]) which identifies the nodes accepted by the waiver
  assignment.
  Takes [Demands] ([nat -> bool]) which identifies the nodes with a grounding demand.
  Takes [i] ([nat]) which is the index of the node under consideration.
  Takes [n] ([node]) which is the node at that index.
  Takes [s] ([schedule]) which is the schedule shared by both runs.
  Takes [w] and [w'] ([world]) which are the external states being compared.
*)
Corollary waived_declaration_sound :
  forall P B C Demands,
    wf P ->
    Assumption2 P ->
    waived_assignment P B C ->
    grounding_accepted Demands C ->
    forall i n s w w',
      nth_error P i = Some n ->
      BlessHolds P B s w w' ->
      carried_world_agrees P C s w w' ->
      Demands i = true ->
      eval P i s w = eval P i s w'.
Proof.
  intros P B C Demands Hwf HA2 Hcov Hacc i n s w w' Hnth Hbless Hcar Hd.
  apply (waived_sound P B C Hwf HA2 Hcov i n s w w' Hnth Hbless Hcar).
  exact (Hacc i Hd).
Qed.

(**
  [theorem2_non_recovery] proves Theorem 2. An [Approx] input gives its consumer an
  [Approx] result lane, including when the input is carried by a recurrence.

  Takes [L] ([nat -> lane]) which is the assignment of lanes to inputs.
  Takes [n] ([node]) which is the consumer.
  Takes [u] ([nat]) which is the index of its ordinary or carried input.
*)
Theorem theorem2_non_recovery :
  forall (L : nat -> lane) (n : node) (u : nat),
    In u (inputs n) \/ carried n = Some u ->
    L u = Approx ->
    node_lane L n = Approx.
Proof.
  intros L n u Hreads Hu.
  assert (H : Approx <=l node_lane L n).
  { rewrite <- Hu.
    destruct Hreads as [Hin | Hc].
    - apply node_lane_ge_input, Hin.
    - apply node_lane_ge_carried, Hc. }

  unfold lane_le in H.
  rewrite join_approx_l in H.
  congruence.
Qed.

(**
  [derivable] requires each node's assigned lane to be equal to or weaker than the
  join of its operation and input lanes.

  Takes [D] ([nat -> lane]) which is the proposed assignment.
  Takes [P] ([prog]) which is the classified graph.
*)
Definition derivable (D : nat -> lane) (P : prog) : Prop :=
  forall i n, nth_error P i = Some n -> node_lane D n <=l D i.

(**
  [has_lane] expresses the join and subsumption rules of Lemma 1 on a classified
  graph. The join rule applies to any node with no carried edge, including a leaf.

  Takes [P] ([prog]) which is the graph to which the rules apply.
*)
Inductive has_lane (P : prog) : nat -> lane -> Prop :=
  (**
    [HL_join] derives a node's lane by joining its operation lane with derivable
    lanes for its ordinary inputs. The rule requires the carried edge to be absent.

    Takes [i] ([nat]) which identifies the result node.
    Takes [n] ([node]) which is the node at that index.
    Takes [f] ([nat -> lane]) which supplies a derivable lane for each input.
  *)
  | HL_join :
      forall i n (f : nat -> lane),
        nth_error P i = Some n ->
        carried n = None ->
        (forall j, In j (inputs n) -> has_lane P j (f j)) ->
        has_lane P i (node_lane f n)

  (**
    [HL_sub] permits a derived lane to be weakened along the lane order.

    Takes [i] ([nat]) which identifies the node.
    Takes [l] and [l'] ([lane]) which are the derived lane and its proposed
    weakening respectively.
  *)
  | HL_sub :
      forall i l l',
        has_lane P i l -> l <=l l' -> has_lane P i l'.

(**
  [acyclic] requires the graph to have no carried edges. Combined with [wf], this
  lets induction over node indices reach every dependency.

  Takes [P] ([prog]) which is the graph being checked.
*)
Definition acyclic (P : prog) : Prop :=
  forall i n, nth_error P i = Some n -> carried n = None.

(**
  [lemma1_sound] proves that the join and subsumption rules derive the assigned lane
  of every node on a well-formed acyclic graph with a sound assignment.

  Takes [P] ([prog]) which is the graph being checked.
  Takes [L] ([nat -> lane]) which is the assignment of lanes to its nodes.
  Takes [i] ([nat]) which is the index of the node under consideration.
  Takes [n] ([node]) which is the node at that index.
*)
Lemma lemma1_sound :
  forall P L,
    wf P ->
    acyclic P ->
    sound_assignment P L ->
    forall i n, nth_error P i = Some n -> has_lane P i (L i).
Proof.
  intros P L Hwf Hacy Hsound i.
  induction i as [i IH] using (well_founded_induction lt_wf).
  intros n Hnth.
  rewrite (Hsound i n Hnth).
  apply (HL_join P i n L Hnth (Hacy i n Hnth)).

  intros j Hin.
  assert (Hlt : j < i) by (eapply Hwf; eauto).
  destruct (nth_error P j) as [nj |] eqn:Hj.
  - apply (IH j Hlt nj Hj).
  - exfalso.
    apply nth_error_None in Hj.
    assert (Hi : i < length P).
    { apply nth_error_Some.
      rewrite Hnth.
      discriminate. }
    lia.
Qed.

(**
  [fold_join_mono] preserves the order between two seeds when the corresponding
  input lanes are also ordered.

  Takes [l] ([list nat]) which holds the input indices.
  Takes [f] and [g] ([nat -> lane]) which give the two lane assignments on those
  indices.
  Takes [a] and [b] ([lane]) which are the respective initial lanes.
*)
Lemma fold_join_mono :
  forall l (f g : nat -> lane) a b,
    (forall x, In x l -> f x <=l g x) ->
    a <=l b ->
    fold_left join (map f l) a <=l fold_left join (map g l) b.
Proof.
  induction l as [| x xs IH]; intros f g a b Hpt Hab; simpl.
  - exact Hab.
  - apply IH.
    + intros y Hin.
      apply Hpt.
      right.
      exact Hin.
    + apply join_lub.
      * eapply le_trans.
        { exact Hab. }
        apply join_ub_l.
      * eapply le_trans.
        { apply Hpt.
          left.
          reflexivity. }
        apply join_ub_r.
Qed.

(**
  [lemma1_least] proves that every lane derived for a node is at least its assigned
  lane under a sound assignment on a well-formed graph.

  Takes [P] ([prog]) which is the graph being checked.
  Takes [L] ([nat -> lane]) which is the assignment of lanes to its nodes.
  Takes [i] ([nat]) which is the index of the node under consideration.
  Takes [l] ([lane]) which is a lane derived for that node.
*)
Lemma lemma1_least :
  forall P L,
    wf P ->
    sound_assignment P L ->
    forall i l, has_lane P i l -> L i <=l l.
Proof.
  intros P L Hwf Hsound i l Hd.
  induction Hd as [i n f Hnth Hnc Hins IH | i l l' Hd IH Hle].
  - rewrite (Hsound i n Hnth).
    unfold node_lane, carried_lane.
    rewrite Hnc.
    apply join_lub.
    + apply exact_bottom.
    + eapply le_trans with (b := fold_left join (map f (inputs n)) (op_lane n)).
      * apply fold_join_mono.
        { intros x Hin.
          apply (IH x Hin). }
        apply le_refl.
      * apply join_ub_r.
  - eapply le_trans.
    + exact IH.
    + exact Hle.
Qed.

(**
  [join_mono] proves that weakening either operand can only weaken their join.

  Takes [a] and [b] ([lane]) which are the original operands.
  Takes [c] and [d] ([lane]) which are their respective upper bounds.
*)
Lemma join_mono :
  forall a b c d, a <=l c -> b <=l d -> join a b <=l join c d.
Proof.
  intros a b c d Hac Hbd.
  apply join_lub.
  - eapply le_trans.
    + exact Hac.
    + apply join_ub_l.
  - eapply le_trans.
    + exact Hbd.
    + apply join_ub_r.
Qed.

(**
  [carried_lane_mono] preserves the order between assignments when reading a carried
  input.

  Takes [L] and [M] ([nat -> lane]) which are the two assignments being compared.
  Takes [n] ([node]) which is the consumer whose lane is computed.
*)
Lemma carried_lane_mono :
  forall (L M : nat -> lane) n,
    (forall j, L j <=l M j) ->
    carried_lane L n <=l carried_lane M n.
Proof.
  intros L M n Hpt.
  unfold carried_lane.
  destruct (carried n).
  - apply Hpt.
  - apply le_refl.
Qed.

(**
  [node_lane_mono] preserves pointwise order between assignments when computing a
  node's result lane.

  Takes [L] and [M] ([nat -> lane]) which are the two assignments being compared.
  Takes [n] ([node]) which is the consumer whose lane is computed.
*)
Lemma node_lane_mono :
  forall (L M : nat -> lane) n,
    (forall j, L j <=l M j) ->
    node_lane L n <=l node_lane M n.
Proof.
  intros L M n Hpt.
  unfold node_lane.
  apply join_mono.
  - apply carried_lane_mono, Hpt.
  - apply fold_join_mono.
    + intros x _.
      apply Hpt.
    + apply le_refl.
Qed.

(**
  [node_lane_ext] gives the same result lane when the assignments agree at every
  index.

  Takes [L] and [M] ([nat -> lane]) which are the two assignments being compared.
  Takes [n] ([node]) which is the consumer whose lane is computed.
*)
Lemma node_lane_ext :
  forall (L M : nat -> lane) n,
    (forall j, L j = M j) ->
    node_lane L n = node_lane M n.
Proof.
  intros L M n Hpt.
  unfold node_lane, carried_lane.
  assert (Hmap : map L (inputs n) = map M (inputs n)) by (apply map_ext; exact Hpt).
  rewrite Hmap.
  destruct (carried n) as [c |]; rewrite ?Hpt; reflexivity.
Qed.

(**
  [step] computes one round of lane inference from the preceding assignment. Indices
  outside the graph receive [Exact].

  Takes [P] ([prog]) which is the graph being classified.
  Takes [L] ([nat -> lane]) which is the assignment from the preceding round.
*)
Definition step (P : prog) (L : nat -> lane) : nat -> lane :=
  fun i =>
    match nth_error P i with
    | Some n => node_lane L n
    | None   => Exact
    end.

(**
  [iter] starts every node at [Exact] and updates all node lanes from the preceding
  assignment on each round.

  Takes [P] ([prog]) which is the classified graph.
  Takes [k] ([nat]) which is the number of completed rounds.
*)
Fixpoint iter (P : prog) (k : nat) : nat -> lane :=
  match k with
  | O    => fun _ => Exact
  | S k' => step P (iter P k')
  end.

(**
  [iter_ascending] shows that each round can only move a node upwards in the lane
  order.

  Takes [P] ([prog]) which is the graph being classified.
  Takes [k] ([nat]) which is the number of completed rounds.
  Takes [i] ([nat]) which identifies the node being examined.
*)
Lemma iter_ascending :
  forall P k i, iter P k i <=l iter P (S k) i.
Proof.
  intros P k.
  induction k as [| k IH]; intros i.
  - apply exact_bottom.
  - simpl.
    unfold step.
    destruct (nth_error P i) as [n |] eqn:Hn.
    + apply node_lane_mono.
      intros j.
      apply IH.
    + apply le_refl.
Qed.

(**
  [iter_stable_forever] proves that once two consecutive assignments agree
  everywhere, further rounds preserve that assignment.

  Takes [P] ([prog]) which is the graph being classified.
  Takes [k] ([nat]) which is the number of completed rounds.
  Takes [m] ([nat]) which is the number of further rounds.
  Takes [i] ([nat]) which identifies the node being examined.
*)
Lemma iter_stable_forever :
  forall P k,
    (forall i, iter P (S k) i = iter P k i) ->
    forall m i, iter P (k + m) i = iter P k i.
Proof.
  intros P k Hstab m.
  induction m as [| m IH]; intros i.
  - rewrite Nat.add_0_r.
    reflexivity.
  - replace (k + S m) with (S (k + m)) by lia.
    simpl.
    unfold step.
    destruct (nth_error P i) as [n |] eqn:Hn.
    + rewrite (node_lane_ext (iter P (k + m)) (iter P k) n IH).
      rewrite <- Hstab.
      simpl.
      unfold step.
      rewrite Hn.
      reflexivity.
    + rewrite <- Hstab.
      simpl.
      unfold step.
      rewrite Hn.
      reflexivity.
Qed.

(**
  [iter_oob] shows that indices beyond the graph keep the initial lane [Exact] in
  every round.

  Takes [P] ([prog]) which is the graph being classified.
  Takes [k] ([nat]) which is the number of completed rounds.
  Takes [i] ([nat]) which is the index outside the graph.
*)
Lemma iter_oob :
  forall P k i, length P <= i -> iter P k i = Exact.
Proof.
  intros P k i Hle.
  destruct k as [| k].
  - reflexivity.
  - simpl.
    unfold step.
    rewrite (proj2 (nth_error_None P i) Hle).
    reflexivity.
Qed.

(**
  [height] counts the strict steps from [Exact] to a lane in the three-element
  order.

  Takes [l] ([lane]) which is the lane being measured.
*)
Definition height (l : lane) : nat :=
  match l with
  | Exact  => 0
  | Replay => 1
  | Approx => 2
  end.

(**
  [height_mono] shows that moving upwards in the lane order can only increase or
  preserve height.

  Takes [a] and [b] ([lane]) which are the two lanes being compared.
*)
Lemma height_mono : forall a b, a <=l b -> height a <= height b.
Proof.
  intros a b H.
  destruct a, b; simpl; try lia; inversion H.
Qed.

(**
  [height_le_2] bounds each node's contribution to the termination measure by two.

  Takes [l] ([lane]) which is the lane whose height is bounded.
*)
Lemma height_le_2 : forall l, height l <= 2.
Proof. destruct l; simpl; lia. Qed.

(**
  [msr] sums the assigned lane heights over the graph's node indices.

  Takes [P] ([prog]) which is the graph being classified.
  Takes [L] ([nat -> lane]) which assigns a lane to each node index.
*)
Definition msr (P : prog) (L : nat -> lane) : nat :=
  fold_right (fun i acc => height (L i) + acc) 0 (seq 0 (length P)).

(**
  [msr_gen_mono] shows that a pointwise increase in lane assignments preserves the
  order of their summed heights.

  Takes [L] and [M] ([nat -> lane]) which are the two assignments being compared.
  Takes [xs] ([list nat]) which holds the node indices included in the sum.
*)
Lemma msr_gen_mono :
  forall (L M : nat -> lane) xs,
    (forall i, L i <=l M i) ->
    fold_right (fun i acc => height (L i) + acc) 0 xs
      <= fold_right (fun i acc => height (M i) + acc) 0 xs.
Proof.
  intros L M xs Hpt.
  induction xs as [| x xs IH]; simpl.
  - lia.
  - specialize (height_mono _ _ (Hpt x)).
    lia.
Qed.

(**
  [msr_gen_eq_pointwise] proves that ordered assignments with equal height sums
  agree at every index included in the sum.

  Takes [L] and [M] ([nat -> lane]) which are the two assignments being compared.
  Takes [xs] ([list nat]) which holds the node indices included in the sum.
*)
Lemma msr_gen_eq_pointwise :
  forall (L M : nat -> lane) xs,
    (forall i, L i <=l M i) ->
    fold_right (fun i acc => height (L i) + acc) 0 xs
      = fold_right (fun i acc => height (M i) + acc) 0 xs ->
    forall i, In i xs -> L i = M i.
Proof.
  intros L M xs Hpt.
  induction xs as [| x xs IH]; simpl; intros Heq i Hin.
  { contradiction. }
  specialize (height_mono _ _ (Hpt x)) as Hx.
  specialize (msr_gen_mono L M xs Hpt) as Hrest.
  assert (Hhx : height (L x) = height (M x)) by lia.
  assert (Htail :
    fold_right (fun j acc => height (L j) + acc) 0 xs =
    fold_right (fun j acc => height (M j) + acc) 0 xs) by lia.

  destruct Hin as [-> | Hin].
  - specialize (Hpt i).
    destruct (L i), (M i); simpl in Hhx;
      try reflexivity; try discriminate; try inversion Hpt.
  - apply IH; assumption.
Qed.

(**
  [msr_bound] bounds the total height by twice the graph's node count.

  Takes [P] ([prog]) which is the graph being classified.
  Takes [L] ([nat -> lane]) which assigns a lane to each node index.
*)
Lemma msr_bound : forall P L, msr P L <= 2 * length P.
Proof.
  intros P L.
  unfold msr.
  assert (H : forall xs,
    fold_right (fun i acc => height (L i) + acc) 0 xs <= 2 * length xs).
  { induction xs as [| x xs IH]; simpl.
    - lia.
    - specialize (height_le_2 (L x)).
      lia. }

  specialize (H (seq 0 (length P))).
  rewrite length_seq in H.
  exact H.
Qed.

(**
  [msr_bottom] gives total height zero when every node starts at [Exact].

  Takes [P] ([prog]) which is the graph being classified.
  Takes [L] ([nat -> lane]) which assigns a lane to each node index.
*)
Lemma msr_bottom : forall P L, (forall i, L i = Exact) -> msr P L = 0.
Proof.
  intros P L HL.
  unfold msr.
  assert (H : forall xs, fold_right (fun i acc => height (L i) + acc) 0 xs = 0).
  { induction xs as [| x xs IH]; simpl.
    - reflexivity.
    - rewrite HL, IH.
      reflexivity. }
  apply H.
Qed.

(**
  [nondecreasing_plateau] finds two consecutive equal values when a nondecreasing
  sequence gains less than one per step on average.

  Takes [f] ([nat -> nat]) which is the sequence.
  Takes [N] ([nat]) which is the number of steps considered.
*)
Lemma nondecreasing_plateau :
  forall (f : nat -> nat) N,
    (forall k, f k <= f (S k)) ->
    f N < f 0 + N ->
    exists k, k < N /\ f k = f (S k).
Proof.
  intros f N Hmono.
  induction N as [| N IH]; intros Hlt.
  { lia. }
  destruct (Nat.eq_dec (f N) (f (S N))) as [Heq | Hne].
  - exists N.
    split.
    + lia.
    + exact Heq.
  - assert (f N < f (S N)) by (specialize (Hmono N); lia).
    destruct (IH ltac:(lia)) as [k [Hk Hfk]].
    exists k.
    split.
    + lia.
    + exact Hfk.
Qed.

(**
  [iter_converges] bounds the number of rounds needed to reach a stable assignment by
  twice the number of nodes. Each node can move up the lane order at most twice.

  Takes [P] ([prog]) which is the graph being checked.
*)
Theorem iter_converges :
  forall P, exists k, k <= 2 * length P /\ forall i, iter P (S k) i = iter P k i.
Proof.
  intros P.
  destruct (nondecreasing_plateau (fun k => msr P (iter P k)) (2 * length P + 1))
    as [k [Hk Heq]].
  - intros k.
    apply msr_gen_mono.
    intros i.
    apply iter_ascending.
  - rewrite (msr_bottom P (iter P 0) (fun _ => eq_refl)).
    specialize (msr_bound P (iter P (2 * length P + 1))).
    lia.
  - exists k.
    split.
    { lia. }
    intros i.
    destruct (le_lt_dec (length P) i) as [Hge | Hlt].
    + rewrite !iter_oob by exact Hge.
      reflexivity.
    + symmetry.
      apply (msr_gen_eq_pointwise (iter P k) (iter P (S k)) (seq 0 (length P))).
      * intros j.
        apply iter_ascending.
      * exact Heq.
      * apply in_seq.
        lia.
Qed.

(**
  [iter_le_derivable] bounds every round of inference by any assignment that
  satisfies the node inequalities.

  Takes [P] ([prog]) which is the graph being classified.
  Takes [D] ([nat -> lane]) which is the proposed upper bound.
  Takes [k] ([nat]) which is the number of completed rounds.
  Takes [i] ([nat]) which identifies the node being examined.
*)
Theorem iter_le_derivable :
  forall P D,
    derivable D P ->
    forall k i, iter P k i <=l D i.
Proof.
  intros P D Hder k.
  induction k as [| k IH]; intros i.
  - apply exact_bottom.
  - simpl.
    unfold step.
    destruct (nth_error P i) as [n |] eqn:Hn.
    + eapply le_trans with (b := node_lane D n).
      * apply node_lane_mono.
        exact IH.
      * apply (Hder i n Hn).
    + apply exact_bottom.
Qed.

(**
  [sound_is_derivable] proves that an assignment satisfying every node equation also
  satisfies its corresponding inequality.

  Takes [P] ([prog]) which is the graph being classified.
  Takes [L] ([nat -> lane]) which assigns a lane to each node index.
*)
Lemma sound_is_derivable :
  forall P L, sound_assignment P L -> derivable L P.
Proof.
  intros P L Hsound i n Hnth.
  rewrite <- (Hsound i n Hnth).
  apply le_refl.
Qed.

(**
  [stable_is_sound] turns an unchanged inference round into an assignment satisfying
  every node equation.

  Takes [P] ([prog]) which is the graph being classified.
  Takes [k] ([nat]) which is the number of completed rounds.
*)
Lemma stable_is_sound :
  forall P k,
    (forall i, iter P (S k) i = iter P k i) ->
    sound_assignment P (iter P k).
Proof.
  intros P k Hstab i n Hnth.
  rewrite <- (Hstab i).
  simpl.
  unfold step.
  rewrite Hnth.
  reflexivity.
Qed.

(**
  [lemma1_least_fixed_point] establishes the existence of a sound assignment below
  every other sound assignment at every node. This form of Lemma 1 also covers graphs
  with carried edges.

  Takes [P] ([prog]) which is the classified graph.
*)
Theorem lemma1_least_fixed_point :
  forall P,
    exists L,
      sound_assignment P L /\
      (forall M, sound_assignment P M -> forall i, L i <=l M i).
Proof.
  intros P.
  destruct (iter_converges P) as [k [_ Hstab]].
  exists (iter P k).
  split.
  - apply stable_is_sound, Hstab.
  - intros M HM i.
    apply iter_le_derivable, sound_is_derivable, HM.
Qed.

(**
  [accepted] checks that the node declares the demand and that its assigned lane is
  strong enough to satisfy it.

  Takes [L] ([nat -> lane]) which is the assignment being checked.
  Takes [n] ([node]) which is the node that carries the declaration.
  Takes [i] ([nat]) which is its index in the assignment.
  Takes [d] ([lane]) which is the declared demand.
*)
Definition accepted (L : nat -> lane) (n : node) (i : nat) (d : lane) : Prop :=
  declared n = Some d /\ L i <=l d.

(**
  [declaration_sound] applies Theorem 1 at an accepted declaration. Agreement at the
  declared lane suffices for equal output bits under the theorem's graph, evaluator
  and recurrence premises.

  Takes [P] ([prog]) which is the graph being checked.
  Takes [L] ([nat -> lane]) which is the assignment of lanes to its nodes.
  Takes [i] ([nat]) which is the index of the node under consideration.
  Takes [n] ([node]) which is the node at that index.
  Takes [d] ([lane]) which is the demand accepted at that node.
  Takes [s] and [s'] ([schedule]) which are the schedules of the two runs.
  Takes [w] ([world]) which is the external state shared by both runs.
*)
Corollary declaration_sound :
  forall P L,
    wf P ->
    Assumption1 P ->
    sound_assignment P L ->
    forall i n d s s' w,
      nth_error P i = Some n ->
      accepted L n i d ->
      carried_agrees P L s s' w ->
      agrees d s s' ->
      eval P i s w = eval P i s' w.
Proof.
  intros P L Hwf HA1 Hsound i n d s s' w Hnth [Hdecl Hacc] Hcar Hag.
  apply (theorem1 P L Hwf HA1 Hsound i n s s' w Hnth Hcar).
  eapply agrees_antitone.
  - exact Hacc.
  - exact Hag.
Qed.
